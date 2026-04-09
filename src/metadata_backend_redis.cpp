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

#include "metadata_backend.h"

#include <atomic>
#include <cstdint>
#include <limits>

#ifdef HAVE_HIREDIS
#include <hiredis/hiredis.h>
#endif

namespace {

class RedisMetadataBackend final : public MetadataBackend
{
public:
    RedisMetadataBackend() : used_bytes(0)
    {
    }

    std::string Name() const override
    {
        return "redis";
    }

    uint64_t GetUsedBytes() const override
    {
        return used_bytes.load(std::memory_order_relaxed);
    }

    bool AddUsedBytesDelta(int64_t delta) override
    {
        uint64_t current = used_bytes.load(std::memory_order_relaxed);
        while(true){
            uint64_t updated;
            if(delta >= 0){
                const uint64_t addend = static_cast<uint64_t>(delta);
                if(current > (std::numeric_limits<uint64_t>::max() - addend)){
                    updated = std::numeric_limits<uint64_t>::max();
                }else{
                    updated = current + addend;
                }
            }else{
                const uint64_t subtrahend = static_cast<uint64_t>(-(delta + 1)) + 1;
                if(subtrahend > current){
                    updated = 0;
                }else{
                    updated = current - subtrahend;
                }
            }
            if(used_bytes.compare_exchange_weak(current, updated, std::memory_order_relaxed, std::memory_order_relaxed)){
                return true;
            }
        }
    }

private:
    std::atomic<uint64_t> used_bytes;
};

#ifdef HAVE_HIREDIS
void ValidateHiredisLinkage()
{
    using RedisFreeFn = void (*)(redisContext*);
    RedisFreeFn volatile redis_free = &redisFree;
    (void)redis_free;
}
#endif

} // namespace

MetadataBackendPtr CreateRedisMetadataBackend(const MetadataBackendConfig& config)
{
    (void)config;

#ifdef HAVE_HIREDIS
    ValidateHiredisLinkage();
    return std::make_unique<RedisMetadataBackend>();
#else
    return nullptr;
#endif
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
