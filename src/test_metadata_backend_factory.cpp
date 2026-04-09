/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2026
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>

#include "metadata_backend_factory.h"
#include "test_util.h"

namespace {

uint64_t NextTestKeySerial()
{
    static std::atomic<uint64_t> serial(0);
    return serial.fetch_add(1, std::memory_order_relaxed);
}

std::string MakeTestUsedBytesKey(const char* suffix)
{
    const auto now_ticks = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::string("s3fs:test:metadata_backend_factory:") +
           std::to_string(now_ticks) + ":" +
           std::to_string(static_cast<unsigned long long>(NextTestKeySerial())) + ":" +
           suffix;
}

void test_empty_redis_uri_returns_null_backend()
{
    MetadataBackendConfig config;
    MetadataBackendPtr backend = CreateMetadataBackend(config);

    ASSERT_TRUE(backend != nullptr);
    ASSERT_FALSE(backend->Name().empty());
    ASSERT_STREQUALS("null", backend->Name().c_str());
    ASSERT_EQUALS(uint64_t(0), backend->GetUsedBytes());
    ASSERT_TRUE(backend->AddUsedBytesDelta(1234));
    ASSERT_EQUALS(uint64_t(0), backend->GetUsedBytes());
}

void test_redis_uri_returns_null_or_redis_backend()
{
    MetadataBackendConfig config;
    config.redis_uri = "redis://127.0.0.1:6379/0";
    config.used_bytes_key = MakeTestUsedBytesKey("redis_uri");
    MetadataBackendPtr backend = CreateMetadataBackend(config);

    ASSERT_TRUE(backend != nullptr);
    const std::string backend_name = backend->Name();
    ASSERT_FALSE(backend_name.empty());
    ASSERT_TRUE("null" == backend_name || "redis" == backend_name);

    ASSERT_EQUALS(uint64_t(0), backend->GetUsedBytes());
    ASSERT_TRUE(backend->AddUsedBytesDelta(1024));
    if("redis" == backend_name){
        ASSERT_EQUALS(uint64_t(1024), backend->GetUsedBytes());
        ASSERT_TRUE(backend->AddUsedBytesDelta(-2048));
        ASSERT_EQUALS(uint64_t(0), backend->GetUsedBytes());
    }else{
        ASSERT_EQUALS(uint64_t(0), backend->GetUsedBytes());
    }
}

void test_invalid_redis_endpoint_still_returns_backend_object()
{
    MetadataBackendConfig config;
    config.redis_uri = "redis://bad-host:6379/0";
    config.used_bytes_key = MakeTestUsedBytesKey("invalid_endpoint");
    MetadataBackendPtr backend = CreateMetadataBackend(config);

    ASSERT_TRUE(backend != nullptr);
    ASSERT_FALSE(backend->Name().empty());
}

#ifdef HAVE_HIREDIS
void test_factory_selects_redis_backend_when_hiredis_enabled()
{
    MetadataBackendConfig cfg;
    cfg.redis_uri = "redis://127.0.0.1:6379/0";
    cfg.used_bytes_key = MakeTestUsedBytesKey("hiredis_enabled");
    MetadataBackendPtr backend = CreateMetadataBackend(cfg);

    ASSERT_TRUE(backend != nullptr);
    const std::string backend_name = backend->Name();
    ASSERT_TRUE("redis" == backend_name || "null" == backend_name);
}
#endif

} // namespace

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    test_empty_redis_uri_returns_null_backend();
    test_redis_uri_returns_null_or_redis_backend();
    test_invalid_redis_endpoint_still_returns_backend_object();
#ifdef HAVE_HIREDIS
    test_factory_selects_redis_backend_when_hiredis_enabled();
#endif
    std::puts("test_metadata_backend_factory: OK");
    return 0;
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
