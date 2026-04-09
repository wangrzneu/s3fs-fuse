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

#include <cstdint>

#include "capacity_policy.h"
#include "test_util.h"

namespace {

static constexpr uint64_t ONE_TIB = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
static constexpr uint64_t ONE_GIB = 1024ULL * 1024ULL * 1024ULL;
static constexpr uint64_t TWO_TIB = 2ULL * ONE_TIB;
static constexpr uint64_t THREE_TIB = 3ULL * ONE_TIB;
static constexpr uint64_t BLOCK_SZ = 16ULL * 1024ULL * 1024ULL;

void test_legacy_mode()
{
    const uint64_t bucket_blocks = 12345;
    const CapacityResult result = ComputeCapacity(CapacityMode::Legacy, bucket_blocks, 123, 456, 4096);

    ASSERT_EQUALS(bucket_blocks, result.f_blocks);
    ASSERT_EQUALS(bucket_blocks, result.f_bfree);
    ASSERT_EQUALS(bucket_blocks, result.f_bavail);
    ASSERT_EQUALS(bucket_blocks * 4096ULL, result.total_bytes);
    ASSERT_EQUALS(bucket_blocks * 4096ULL, result.free_bytes);
}

void test_redis_mode_default_size()
{
    const CapacityResult result = ComputeCapacity(CapacityMode::Redis, 0, 0, 0, BLOCK_SZ);

    ASSERT_EQUALS(ONE_TIB, result.total_bytes);
    ASSERT_EQUALS(ONE_TIB, result.free_bytes);
    ASSERT_EQUALS(ONE_TIB / BLOCK_SZ, result.f_blocks);
    ASSERT_EQUALS(ONE_TIB / BLOCK_SZ, result.f_bfree);
    ASSERT_EQUALS(ONE_TIB / BLOCK_SZ, result.f_bavail);
}

void test_redis_mode_used_capacity()
{
    const CapacityResult result = ComputeCapacity(CapacityMode::Redis, 0, ONE_TIB, 512ULL * ONE_GIB, BLOCK_SZ);

    ASSERT_EQUALS(ONE_TIB, result.total_bytes);
    ASSERT_EQUALS(512ULL * ONE_GIB, result.free_bytes);
    ASSERT_EQUALS(ONE_TIB / BLOCK_SZ, result.f_blocks);
    ASSERT_EQUALS((512ULL * ONE_GIB) / BLOCK_SZ, result.f_bfree);
    ASSERT_EQUALS(result.f_bfree, result.f_bavail);
}

void test_redis_mode_saturation()
{
    const CapacityResult result = ComputeCapacity(CapacityMode::Redis, 0, ONE_TIB, ONE_TIB + ONE_GIB, BLOCK_SZ);

    ASSERT_EQUALS(ONE_TIB, result.total_bytes);
    ASSERT_EQUALS(uint64_t(0), result.free_bytes);
    ASSERT_EQUALS(ONE_TIB / BLOCK_SZ, result.f_blocks);
    ASSERT_EQUALS(uint64_t(0), result.f_bfree);
    ASSERT_EQUALS(uint64_t(0), result.f_bavail);
}

void test_redis_mode_saturation_at_two_tib()
{
    const CapacityResult result = ComputeCapacity(CapacityMode::Redis, 0, TWO_TIB, THREE_TIB, BLOCK_SZ);

    ASSERT_EQUALS(TWO_TIB, result.total_bytes);
    ASSERT_EQUALS(uint64_t(0), result.free_bytes);
    ASSERT_EQUALS(TWO_TIB / BLOCK_SZ, result.f_blocks);
    ASSERT_EQUALS(uint64_t(0), result.f_bfree);
    ASSERT_EQUALS(uint64_t(0), result.f_bavail);
}

}

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    test_legacy_mode();
    test_redis_mode_default_size();
    test_redis_mode_used_capacity();
    test_redis_mode_saturation();
    test_redis_mode_saturation_at_two_tib();
    std::puts("test_capacity_policy: OK");
    return 0;
}
