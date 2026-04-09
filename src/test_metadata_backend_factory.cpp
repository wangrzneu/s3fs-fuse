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

#include <cstdio>
#include <string>

#include "metadata_backend_factory.h"
#include "test_util.h"

MetadataBackendPtr CreateRedisMetadataBackend(const MetadataBackendConfig& config);

namespace {

void test_empty_redis_uri_returns_null_backend()
{
    MetadataBackendConfig config;
    MetadataBackendPtr backend = CreateMetadataBackend(config);

    ASSERT_TRUE(backend != nullptr);
    ASSERT_FALSE(backend->Name().empty());
    ASSERT_STREQUALS("null", backend->Name().c_str());
}

void test_redis_uri_returns_null_or_redis_backend()
{
    MetadataBackendConfig config;
    config.redis_uri = "redis://127.0.0.1:6379/0";
    MetadataBackendPtr backend = CreateMetadataBackend(config);

    ASSERT_TRUE(backend != nullptr);
    const std::string backend_name = backend->Name();
    ASSERT_FALSE(backend_name.empty());
    ASSERT_TRUE("null" == backend_name || "redis" == backend_name);
}

#ifdef HAVE_HIREDIS
void test_direct_redis_backend_creation_returns_redis()
{
    MetadataBackendConfig config;
    config.redis_uri = "redis://127.0.0.1:6379/0";
    MetadataBackendPtr backend = CreateRedisMetadataBackend(config);

    ASSERT_TRUE(backend != nullptr);
    ASSERT_STREQUALS("redis", backend->Name().c_str());
}
#endif

} // namespace

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    test_empty_redis_uri_returns_null_backend();
    test_redis_uri_returns_null_or_redis_backend();
#ifdef HAVE_HIREDIS
    test_direct_redis_backend_creation_returns_redis();
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
