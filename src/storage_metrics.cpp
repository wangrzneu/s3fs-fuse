/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Randy Rizun <rrizun@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <string>

#include "common.h"
#include "curl.h"
#include "s3fs_logger.h"
#include "s3fs_xml.h"
#include "s3fs_util.h"
#include "string_util.h"
#include "s3fs_threadreqs.h"
#include "storage_metrics.h"

//------------------------------------------------
// Static members
//------------------------------------------------
std::unique_ptr<StorageMetrics> StorageMetrics::singleton;

//------------------------------------------------
// Helper: extract total size from a ListObjectsV2 XML response
//------------------------------------------------
// Parses <Contents><Size>NNN</Size></Contents> elements and sums them.
// Also returns truncation status and continuation token via out-params.
//
static int64_t sum_object_sizes_from_xml(const std::string& body, bool& is_truncated_out, std::string& next_token_out)
{
    is_truncated_out = false;
    next_token_out.clear();

    if(body.empty()){
        return 0;
    }

    std::string encbody = get_encoded_cr_code(body.c_str());
    if(encbody.empty()){
        return 0;
    }

    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlReadMemory(encbody.c_str(), static_cast<int>(encbody.size()), "", nullptr, 0),
        xmlFreeDoc
    );
    if(!doc){
        S3FS_PRN_ERR("StorageMetrics: xmlReadMemory failed.");
        return -1;
    }

    // Check IsTruncated
    {
        is_truncated_out = is_truncated(doc.get());
    }

    // Get NextContinuationToken (for ListObjectsV2)
    {
        auto token = get_next_continuation_token(doc.get());
        if(token){
            next_token_out = reinterpret_cast<const char*>(token.get());
        }else{
            // Fall back to NextMarker (for ListObjects v1)
            auto marker = get_next_marker(doc.get());
            if(marker){
                next_token_out = reinterpret_cast<const char*>(marker.get());
            }
        }
    }

    // Sum up all <Size> elements under <Contents>
    unique_ptr_xmlXPathContext ctx(xmlXPathNewContext(doc.get()), xmlXPathFreeContext);
    if(!ctx){
        return -1;
    }

    // Detect XML namespace
    std::string xmlnsurl;
    std::string size_xpath;

    xmlNodePtr root = xmlDocGetRootElement(doc.get());
    if(root && root->ns && root->ns->href){
        xmlnsurl = reinterpret_cast<const char*>(root->ns->href);
    }

    if(!noxmlns && !xmlnsurl.empty()){
        xmlXPathRegisterNs(ctx.get(), reinterpret_cast<const xmlChar*>("s3"),
                           reinterpret_cast<const xmlChar*>(xmlnsurl.c_str()));
        size_xpath = "//s3:Contents/s3:Size";
    }else{
        size_xpath = "//Contents/Size";
    }

    unique_ptr_xmlXPathObject result(
        xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(size_xpath.c_str()), ctx.get()),
        xmlXPathFreeObject
    );
    if(!result || xmlXPathNodeSetIsEmpty(result->nodesetval)){
        return 0;
    }

    int64_t total = 0;
    xmlNodeSetPtr nodes = result->nodesetval;
    for(int i = 0; i < nodes->nodeNr; i++){
        std::unique_ptr<xmlChar, decltype(xmlFree)> content(
            xmlNodeListGetString(doc.get(), nodes->nodeTab[i]->xmlChildrenNode, 1),
            xmlFree
        );
        if(content){
            total += strtoll(reinterpret_cast<const char*>(content.get()), nullptr, 10);
        }
    }

    return total;
}

//------------------------------------------------
// StorageMetrics implementation
//------------------------------------------------

StorageMetrics::~StorageMetrics()
{
    exit_flag.store(true);
    if(scan_thread.joinable()){
        scan_thread.join();
    }
}

int64_t StorageMetrics::ScanBucketSize()
{
    S3FS_PRN_INFO("StorageMetrics: starting bucket size scan...");

    int64_t total_bytes = 0;
    bool truncated = true;
    std::string next_token;

    while(truncated && !exit_flag.load()){
        std::string query;

        // Build query parameters in alphabetical order
        if(!next_token.empty()){
            if(S3fsCurl::IsListObjectsV2()){
                query += "continuation-token=" + urlEncodePath(next_token) + "&";
            }else{
                query += "marker=" + urlEncodePath(next_token) + "&";
            }
        }
        if(S3fsCurl::IsListObjectsV2()){
            query += "list-type=2&";
        }
        query += "max-keys=1000";

        // Make the request
        std::string responseBody;
        int result;
        if(0 != (result = list_bucket_request("", query, responseBody))){
            S3FS_PRN_ERR("StorageMetrics: list_bucket_request failed with %d", result);
            return -1;
        }

        // Parse sizes from response
        bool is_trunc = false;
        std::string token;
        int64_t page_size = sum_object_sizes_from_xml(responseBody, is_trunc, token);
        if(page_size < 0){
            S3FS_PRN_ERR("StorageMetrics: failed to parse XML response.");
            return -1;
        }

        total_bytes += page_size;
        truncated = is_trunc;
        next_token = token;
    }

    if(exit_flag.load()){
        S3FS_PRN_INFO("StorageMetrics: scan aborted due to exit.");
        return -1;
    }

    S3FS_PRN_INFO("StorageMetrics: bucket size scan complete. Total used: %lld bytes", static_cast<long long>(total_bytes));
    return total_bytes;
}

void StorageMetrics::ScanWorker()
{
    while(!exit_flag.load()){
        int64_t scanned = ScanBucketSize();
        if(scanned >= 0){
            std::lock_guard<std::mutex> lock(scan_mutex);
            used_bytes.store(scanned);
            if(!scan_completed.load()){
                scan_completed.store(true);
                S3FS_PRN_INFO("StorageMetrics: initial scan complete. Used bytes: %lld", static_cast<long long>(scanned));
            }else{
                S3FS_PRN_INFO("StorageMetrics: re-scan complete. Used bytes: %lld", static_cast<long long>(scanned));
            }
        }else{
            S3FS_PRN_WARN("StorageMetrics: scan failed, will retry on next interval.");
        }

        // Wait for rescan interval, checking exit_flag periodically
        for(int i = 0; i < rescan_interval_sec && !exit_flag.load(); i++){
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

bool StorageMetrics::Initialize(bool enable, int rescan_interval)
{
    if(singleton){
        S3FS_PRN_WARN("StorageMetrics: already initialized.");
        return true;
    }
    singleton = std::unique_ptr<StorageMetrics>(new StorageMetrics());
    singleton->enabled = enable;
    singleton->rescan_interval_sec = rescan_interval;

    if(enable){
        singleton->exit_flag.store(false);
        singleton->scan_thread = std::thread(&StorageMetrics::ScanWorker, singleton.get());
        S3FS_PRN_INFO("StorageMetrics: initialized with rescan interval %d seconds.", rescan_interval);
    }else{
        S3FS_PRN_INFO("StorageMetrics: disabled.");
    }
    return true;
}

void StorageMetrics::Destroy()
{
    if(singleton){
        singleton->exit_flag.store(true);
        if(singleton->scan_thread.joinable()){
            singleton->scan_thread.join();
        }
        singleton.reset();
        S3FS_PRN_INFO("StorageMetrics: destroyed.");
    }
}

bool StorageMetrics::IsEnabled()
{
    return singleton && singleton->enabled;
}

bool StorageMetrics::IsScanComplete()
{
    return singleton && singleton->scan_completed.load();
}

int64_t StorageMetrics::GetUsedBytes()
{
    if(!singleton){
        return 0;
    }
    return singleton->used_bytes.load();
}

void StorageMetrics::AddBytes(int64_t bytes)
{
    if(singleton && singleton->enabled && singleton->scan_completed.load()){
        singleton->used_bytes.fetch_add(bytes);
    }
}

void StorageMetrics::SubBytes(int64_t bytes)
{
    if(singleton && singleton->enabled && singleton->scan_completed.load()){
        singleton->used_bytes.fetch_sub(bytes);
    }
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
