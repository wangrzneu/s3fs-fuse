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

#include "capacity_policy.h"

#include <cstring>

namespace {

static constexpr uint64_t REDIS_DEFAULT_BUCKET_SIZE_BYTES = 1024ULL * 1024ULL * 1024ULL * 1024ULL;

uint64_t normalize_block_size(uint64_t block_size)
{
    return (0 == block_size) ? 1 : block_size;
}

uint64_t bytes_to_blocks(uint64_t bytes, uint64_t block_size)
{
    return bytes / normalize_block_size(block_size);
}

}

bool ParseCapacityMode(const char* value, CapacityMode& out_mode)
{
    if(nullptr == value){
        return false;
    }

    if(0 == strcmp(value, "legacy")){
        out_mode = CapacityMode::Legacy;
        return true;
    }

    if(0 == strcmp(value, "redis")){
        out_mode = CapacityMode::Redis;
        return true;
    }

    return false;
}

uint64_t ComputeEffectiveBucketSizeBytes(CapacityMode mode, bool is_bucket_size_explicit, uint64_t bucket_blocks, uint64_t block_size)
{
    if(CapacityMode::Redis == mode && !is_bucket_size_explicit){
        return 0;
    }

    return bucket_blocks * block_size;
}

CapacityResult ComputeCapacity(CapacityMode mode, uint64_t bucket_blocks, uint64_t bucket_size_bytes, uint64_t used_bytes, uint64_t block_size)
{
    CapacityResult result{};
    const uint64_t effective_block_size = normalize_block_size(block_size);

    if(CapacityMode::Legacy == mode){
        result.f_blocks    = bucket_blocks;
        result.f_bfree     = bucket_blocks;
        result.f_bavail    = bucket_blocks;
        result.total_bytes = bucket_blocks * effective_block_size;
        result.free_bytes  = result.total_bytes;
        return result;
    }

    result.total_bytes = (0 == bucket_size_bytes) ? REDIS_DEFAULT_BUCKET_SIZE_BYTES : bucket_size_bytes;
    result.free_bytes  = (used_bytes >= result.total_bytes) ? 0 : (result.total_bytes - used_bytes);
    result.f_blocks    = bytes_to_blocks(result.total_bytes, effective_block_size);
    result.f_bfree     = bytes_to_blocks(result.free_bytes, effective_block_size);
    result.f_bavail    = result.f_bfree;
    return result;
}
