/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef S3FS_CAPACITY_POLICY_H_
#define S3FS_CAPACITY_POLICY_H_

#include <cstdint>

enum class CapacityMode {
    Legacy,
    Redis
};

struct CapacityResult
{
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t total_bytes;
    uint64_t free_bytes;
};

bool ParseCapacityMode(const char* value, CapacityMode& out_mode);
uint64_t ComputeEffectiveBucketSizeBytes(CapacityMode mode, bool is_bucket_size_explicit, uint64_t bucket_blocks, uint64_t block_size);
CapacityResult ComputeCapacity(CapacityMode mode, uint64_t bucket_blocks, uint64_t bucket_size_bytes, uint64_t used_bytes, uint64_t block_size);

#endif // S3FS_CAPACITY_POLICY_H_

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
