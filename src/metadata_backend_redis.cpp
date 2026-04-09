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

#ifdef HAVE_HIREDIS
#include <hiredis/hiredis.h>
#endif

namespace {

class RedisMetadataBackend final : public MetadataBackend
{
public:
    std::string Name() const override
    {
        return "redis";
    }
};

} // namespace

MetadataBackendPtr CreateRedisMetadataBackend(const MetadataBackendConfig& config)
{
    (void)config;

#ifdef HAVE_HIREDIS
    (void)sizeof(redisContext);
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
