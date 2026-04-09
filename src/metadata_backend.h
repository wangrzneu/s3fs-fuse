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

#ifndef S3FS_METADATA_BACKEND_H_
#define S3FS_METADATA_BACKEND_H_

#include <cstdint>
#include <memory>
#include <string>

struct MetadataBackendConfig
{
    std::string redis_uri;
    std::string used_bytes_key;
    long connect_timeout_ms = 100;
    long rw_timeout_ms = 100;
};

class MetadataBackend
{
public:
    virtual ~MetadataBackend() = default;

    virtual std::string Name() const = 0;
    virtual uint64_t GetUsedBytes() const = 0;
    virtual bool AddUsedBytesDelta(int64_t delta) = 0;
};

using MetadataBackendPtr = std::unique_ptr<MetadataBackend>;

#endif // S3FS_METADATA_BACKEND_H_

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
