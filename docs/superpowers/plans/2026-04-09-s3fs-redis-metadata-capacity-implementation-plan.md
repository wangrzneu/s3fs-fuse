# s3fs Redis Metadata And Capacity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional Redis-backed metadata L2 caching and dynamic `statfs` capacity display (`capacity_mode=redis`) while keeping default behavior identical to upstream.

**Architecture:** Keep current in-process `StatCache` as L1 and add a pluggable L2 metadata backend. Implement Redis backend as optional build/runtime feature with graceful fallback. Keep `s3fs.cpp` changes thin by moving capacity logic and backend wiring into focused new modules.

**Tech Stack:** C++14, FUSE3/FUSE-T, existing s3fs cache tree, optional hiredis, autotools, existing unit/integration test style.

---

## File Structure And Responsibilities

### New files

- `src/capacity_policy.h`
  Defines capacity mode enum and pure functions for block calculation from `bucket_size` and `used_bytes`.
- `src/capacity_policy.cpp`
  Implements capacity calculations and default `1T` behavior for `capacity_mode=redis`.
- `src/metadata_backend.h`
  Declares L2 metadata backend interface and DTOs for stat/meta/dirlist payloads.
- `src/metadata_backend_null.cpp`
  Default no-op backend implementation used when Redis is disabled/unconfigured.
- `src/metadata_backend_redis.cpp`
  Optional hiredis-backed implementation.
- `src/metadata_backend_factory.h`
  Creates backend from mount options and compile-time capability.
- `src/metadata_backend_factory.cpp`
  Factory implementation and fallback decision logic.
- `src/test_capacity_policy.cpp`
  Unit tests for capacity math and default `1T` behavior.
- `src/test_metadata_backend_factory.cpp`
  Unit tests for backend selection and fallback behavior.

### Modified files

- `src/s3fs.cpp`
  Parse `capacity_mode` and Redis options; delegate `statfs` math to `capacity_policy`; initialize metadata backend once at startup.
- `src/cache.h`
  Add backend setter and internal L2 helpers.
- `src/cache.cpp`
  Integrate L1/L2 read-through and write-through for stat, dirlist, negative entries.
- `src/Makefile.am`
  Add new source files and new unit test binaries.
- `configure.ac`
  Add optional hiredis dependency detection and `HAVE_HIREDIS` compile flag.
- `doc/man/s3fs.1.in`
  Document new options and `capacity_mode=redis` + `bucket_size` semantics.

### Existing tests touched

- `src/Makefile.am`
  Register `test_capacity_policy` and `test_metadata_backend_factory` in `TESTS`.

---

### Task 1: Add Capacity Policy Module (TDD)

**Files:**
- Create: `src/capacity_policy.h`
- Create: `src/capacity_policy.cpp`
- Create: `src/test_capacity_policy.cpp`
- Modify: `src/Makefile.am`

- [ ] **Step 1: Write the failing test**

```cpp
// src/test_capacity_policy.cpp
#include <cassert>
#include <cstdint>
#include <iostream>

#include "capacity_policy.h"

int main()
{
    constexpr uint64_t block = 16ULL * 1024ULL * 1024ULL;
    {
        CapacityResult r = ComputeCapacity(CapacityMode::Legacy, /*bucket_blocks=*/1024, /*bucket_size_bytes=*/0, /*used_bytes=*/0, block);
        assert(r.f_blocks == 1024);
        assert(r.f_bfree == 1024);
        assert(r.f_bavail == 1024);
    }
    {
        CapacityResult r = ComputeCapacity(CapacityMode::Redis, /*bucket_blocks=*/0, /*bucket_size_bytes=*/0, /*used_bytes=*/0, block);
        // redis mode defaults to 1T when bucket_size is unset
        assert(r.total_bytes == (1ULL << 40));
        assert(r.free_bytes == (1ULL << 40));
    }
    {
        CapacityResult r = ComputeCapacity(CapacityMode::Redis, /*bucket_blocks=*/0, /*bucket_size_bytes=*/(1ULL << 40), /*used_bytes=*/(512ULL << 30), block);
        assert(r.total_bytes == (1ULL << 40));
        assert(r.free_bytes  == (512ULL << 30));
        assert(r.f_bfree == r.f_bavail);
    }
    std::cout << "test_capacity_policy: OK\n";
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C src test_capacity_policy`  
Expected: FAIL with missing include/source errors for `capacity_policy.h/.cpp`.

- [ ] **Step 3: Write minimal implementation**

```cpp
// src/capacity_policy.h
#ifndef S3FS_CAPACITY_POLICY_H_
#define S3FS_CAPACITY_POLICY_H_

#include <cstdint>

enum class CapacityMode { Legacy, Redis };

struct CapacityResult
{
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t total_bytes;
    uint64_t free_bytes;
};

CapacityResult ComputeCapacity(CapacityMode mode, uint64_t bucket_blocks, uint64_t bucket_size_bytes, uint64_t used_bytes, uint64_t block_size);

#endif
```

```cpp
// src/capacity_policy.cpp
#include "capacity_policy.h"

#include <algorithm>

namespace {
constexpr uint64_t kDefaultRedisBucketSizeBytes = (1ULL << 40); // 1T
}

CapacityResult ComputeCapacity(CapacityMode mode, uint64_t bucket_blocks, uint64_t bucket_size_bytes, uint64_t used_bytes, uint64_t block_size)
{
    if(mode == CapacityMode::Legacy){
        return CapacityResult{bucket_blocks, bucket_blocks, bucket_blocks, bucket_blocks * block_size, bucket_blocks * block_size};
    }
    const uint64_t total = (bucket_size_bytes == 0 ? kDefaultRedisBucketSizeBytes : bucket_size_bytes);
    const uint64_t free  = (used_bytes >= total ? 0 : (total - used_bytes));
    const uint64_t blk   = (block_size == 0 ? 1 : block_size);
    const uint64_t fblk  = total / blk;
    const uint64_t ffree = free / blk;
    return CapacityResult{fblk, ffree, ffree, total, free};
}
```

```makefile
# src/Makefile.am (append)
noinst_PROGRAMS += test_capacity_policy
test_capacity_policy_SOURCES = capacity_policy.cpp test_capacity_policy.cpp
TESTS += test_capacity_policy
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make -C src test_capacity_policy && ./src/test_capacity_policy`  
Expected: PASS and output `test_capacity_policy: OK`.

- [ ] **Step 5: Commit**

```bash
git add src/capacity_policy.h src/capacity_policy.cpp src/test_capacity_policy.cpp src/Makefile.am
git commit -m "feat: add capacity policy module with redis default 1T behavior"
```

---

### Task 2: Wire Capacity Policy Into `s3fs_statfs` (TDD)

**Files:**
- Modify: `src/s3fs.cpp`
- Test: `src/test_capacity_policy.cpp`

- [ ] **Step 1: Write failing test case for statfs mapping contract**

```cpp
// add into src/test_capacity_policy.cpp
{
    CapacityResult r = ComputeCapacity(CapacityMode::Redis, /*bucket_blocks=*/0, /*bucket_size_bytes=*/(2ULL << 40), /*used_bytes=*/(3ULL << 40), block);
    assert(r.free_bytes == 0);
    assert(r.f_bfree == 0);
}
```

- [ ] **Step 2: Run test to verify failure (if logic not clamped)**

Run: `make -C src test_capacity_policy && ./src/test_capacity_policy`  
Expected: FAIL if underflow clamp is missing.

- [ ] **Step 3: Implement `s3fs.cpp` wiring**

```cpp
// src/s3fs.cpp (key snippets)
#include "capacity_policy.h"

static CapacityMode capacity_mode = CapacityMode::Legacy;
static bool is_bucket_size_explicit = false;

// in option parse path
else if(is_prefix(arg, "capacity_mode=")){
    const char* mode = strchr(arg, '=') + 1;
    if(strcmp(mode, "legacy") == 0){
        capacity_mode = CapacityMode::Legacy;
    }else if(strcmp(mode, "redis") == 0){
        capacity_mode = CapacityMode::Redis;
    }else{
        S3FS_PRN_EXIT("unknown capacity_mode option.");
        return -1;
    }
    return 0;
}
else if(is_prefix(arg, "bucket_size=")){
    bucket_block_count = parse_bucket_size(strchr(arg, '=') + sizeof(char));
    is_bucket_size_explicit = true;
    if(0 == bucket_block_count){
        S3FS_PRN_EXIT("invalid bucket_size option.");
        return -1;
    }
    return 0;
}

// in s3fs_statfs
uint64_t bucket_size_bytes = static_cast<uint64_t>(bucket_block_count) * static_cast<uint64_t>(s3fs_block_size);
if(capacity_mode == CapacityMode::Redis && !is_bucket_size_explicit){
    bucket_size_bytes = 0; // ComputeCapacity applies 1T default
}
CapacityResult cap = ComputeCapacity(capacity_mode, bucket_block_count, bucket_size_bytes, /*used_bytes=*/GetCapacityUsedBytes(), s3fs_block_size);
stbuf->f_blocks = cap.f_blocks;
stbuf->f_bfree  = cap.f_bfree;
stbuf->f_bavail = cap.f_bavail;
```

- [ ] **Step 4: Run tests**

Run: `make -C src test_capacity_policy && ./src/test_capacity_policy`  
Expected: PASS.

Run: `make -C src test_curl_util test_page_list test_string_util`  
Expected: PASS (no regression in existing src unit tests).

- [ ] **Step 5: Commit**

```bash
git add src/s3fs.cpp src/test_capacity_policy.cpp
git commit -m "feat: add capacity_mode and wire statfs capacity policy"
```

---

### Task 3: Add Metadata Backend Interface + Null Backend (TDD)

**Files:**
- Create: `src/metadata_backend.h`
- Create: `src/metadata_backend_null.cpp`
- Create: `src/metadata_backend_factory.h`
- Create: `src/metadata_backend_factory.cpp`
- Create: `src/test_metadata_backend_factory.cpp`
- Modify: `src/Makefile.am`

- [ ] **Step 1: Write failing tests for backend selection**

```cpp
// src/test_metadata_backend_factory.cpp
#include <cassert>
#include <memory>
#include <string>
#include "metadata_backend_factory.h"

int main()
{
    MetadataBackendConfig c1;
    c1.redis_uri = "";
    auto b1 = CreateMetadataBackend(c1);
    assert(b1->Name() == "null");

    MetadataBackendConfig c2;
    c2.redis_uri = "redis://127.0.0.1:6379/0";
    auto b2 = CreateMetadataBackend(c2);
    // without hiredis build flag this still must fallback to null
    assert(!b2->Name().empty());
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C src test_metadata_backend_factory`  
Expected: FAIL with missing backend headers/sources.

- [ ] **Step 3: Write minimal interface and null backend**

```cpp
// src/metadata_backend.h
#ifndef S3FS_METADATA_BACKEND_H_
#define S3FS_METADATA_BACKEND_H_

#include <string>

struct MetadataBackendConfig
{
    std::string redis_uri;
    long connect_timeout_ms = 100;
    long rw_timeout_ms = 100;
};

class MetadataBackend
{
public:
    virtual ~MetadataBackend() = default;
    virtual std::string Name() const = 0;
};

#endif
```

```cpp
// src/metadata_backend_null.cpp
#include "metadata_backend.h"

class NullMetadataBackend final : public MetadataBackend
{
public:
    std::string Name() const override { return "null"; }
};

std::unique_ptr<MetadataBackend> CreateNullMetadataBackend()
{
    return std::unique_ptr<MetadataBackend>(new NullMetadataBackend());
}
```

```cpp
// src/metadata_backend_factory.h
#ifndef S3FS_METADATA_BACKEND_FACTORY_H_
#define S3FS_METADATA_BACKEND_FACTORY_H_

#include <memory>
#include "metadata_backend.h"

std::unique_ptr<MetadataBackend> CreateMetadataBackend(const MetadataBackendConfig& config);
std::unique_ptr<MetadataBackend> CreateNullMetadataBackend();

#endif
```

```cpp
// src/metadata_backend_factory.cpp
#include "metadata_backend_factory.h"

std::unique_ptr<MetadataBackend> CreateMetadataBackend(const MetadataBackendConfig& config)
{
    (void)config;
    return CreateNullMetadataBackend();
}
```

```makefile
# src/Makefile.am (append)
s3fs_SOURCES += metadata_backend_factory.cpp metadata_backend_null.cpp
noinst_PROGRAMS += test_metadata_backend_factory
test_metadata_backend_factory_SOURCES = metadata_backend_factory.cpp metadata_backend_null.cpp test_metadata_backend_factory.cpp
TESTS += test_metadata_backend_factory
```

- [ ] **Step 4: Run tests**

Run: `make -C src test_metadata_backend_factory && ./src/test_metadata_backend_factory`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/metadata_backend.h src/metadata_backend_null.cpp src/metadata_backend_factory.h src/metadata_backend_factory.cpp src/test_metadata_backend_factory.cpp src/Makefile.am
git commit -m "feat: add metadata backend interface and null backend factory"
```

---

### Task 4: Add Optional hiredis Backend + Build Flag (TDD)

**Files:**
- Modify: `configure.ac`
- Modify: `src/Makefile.am`
- Create: `src/metadata_backend_redis.cpp`
- Modify: `src/metadata_backend_factory.cpp`
- Test: `src/test_metadata_backend_factory.cpp`

- [ ] **Step 1: Write failing compile-path test for redis selection**

```cpp
// append into src/test_metadata_backend_factory.cpp
MetadataBackendConfig c3;
c3.redis_uri = "redis://127.0.0.1:6379/0";
auto b3 = CreateMetadataBackend(c3);
// Name is "redis" only when built with hiredis support and backend init succeeds.
assert(b3->Name() == "null" || b3->Name() == "redis");
```

- [ ] **Step 2: Run test to verify current behavior**

Run: `make -C src test_metadata_backend_factory && ./src/test_metadata_backend_factory`  
Expected: PASS but always `null` before redis backend is wired.

- [ ] **Step 3: Implement optional hiredis build and redis backend**

```m4
# configure.ac (key addition)
AC_ARG_WITH([hiredis], [AS_HELP_STRING([--with-hiredis], [enable redis metadata backend])], [], [with_hiredis=auto])
AS_IF([test "x$with_hiredis" != "xno"], [
  PKG_CHECK_MODULES([HIREDIS], [hiredis >= 1.0.0], [AC_DEFINE([HAVE_HIREDIS],[1],[have hiredis])], [AS_IF([test "x$with_hiredis" = "xyes"], [AC_MSG_ERROR([hiredis requested but not found])])])
])
AC_SUBST([HIREDIS_CFLAGS])
AC_SUBST([HIREDIS_LIBS])
```

```makefile
# src/Makefile.am (key addition)
AM_CPPFLAGS += $(HIREDIS_CFLAGS)
s3fs_LDADD += $(HIREDIS_LIBS)
s3fs_SOURCES += metadata_backend_redis.cpp
test_metadata_backend_factory_SOURCES += metadata_backend_redis.cpp
```

```cpp
// src/metadata_backend_redis.cpp (minimal)
#include "metadata_backend.h"

class RedisMetadataBackend final : public MetadataBackend
{
public:
    std::string Name() const override { return "redis"; }
};

std::unique_ptr<MetadataBackend> CreateRedisMetadataBackend(const MetadataBackendConfig&)
{
#ifdef HAVE_HIREDIS
    return std::unique_ptr<MetadataBackend>(new RedisMetadataBackend());
#else
    return nullptr;
#endif
}
```

```cpp
// src/metadata_backend_factory.cpp (update)
std::unique_ptr<MetadataBackend> CreateRedisMetadataBackend(const MetadataBackendConfig&);
std::unique_ptr<MetadataBackend> CreateMetadataBackend(const MetadataBackendConfig& config)
{
    if(!config.redis_uri.empty()){
        if(auto redis = CreateRedisMetadataBackend(config)){
            return redis;
        }
    }
    return CreateNullMetadataBackend();
}
```

- [ ] **Step 4: Run build/tests**

Run: `./autogen.sh && ./configure`  
Expected: PASS with or without hiredis installed.

Run: `make -C src test_metadata_backend_factory && ./src/test_metadata_backend_factory`  
Expected: PASS (`null` without hiredis, `redis` possible with hiredis).

- [ ] **Step 5: Commit**

```bash
git add configure.ac src/Makefile.am src/metadata_backend_redis.cpp src/metadata_backend_factory.cpp src/test_metadata_backend_factory.cpp
git commit -m "feat: add optional hiredis metadata backend"
```

---

### Task 5: Integrate L2 Backend Into StatCache (TDD)

**Files:**
- Modify: `src/cache.h`
- Modify: `src/cache.cpp`
- Modify: `src/s3fs.cpp`
- Test: `src/test_metadata_backend_factory.cpp`

- [ ] **Step 1: Write failing behavior test for fallback path contract**

```cpp
// append in src/test_metadata_backend_factory.cpp
MetadataBackendConfig c4;
c4.redis_uri = "redis://bad-host:6379/0";
auto b4 = CreateMetadataBackend(c4);
assert(!b4->Name().empty()); // must never crash or return null pointer
```

- [ ] **Step 2: Run test**

Run: `make -C src test_metadata_backend_factory && ./src/test_metadata_backend_factory`  
Expected: PASS for pointer safety; later steps enforce runtime fallback in logs.

- [ ] **Step 3: Implement StatCache L2 hooks**

```cpp
// src/cache.h (key additions)
class MetadataBackend;
class StatCache
{
  // ...
  public:
    static void SetMetadataBackend(std::shared_ptr<MetadataBackend> backend);
};
```

```cpp
// src/cache.cpp (key additions)
static std::shared_ptr<MetadataBackend> g_metadata_backend;

void StatCache::SetMetadataBackend(std::shared_ptr<MetadataBackend> backend)
{
    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);
    g_metadata_backend = std::move(backend);
}

// In GetStat/GetS3ObjList:
// L1 miss => try backend lookup => if hit then AddStat/AddS3ObjList to L1 and return.
// In AddStat/AddS3ObjList/DelStat/AddNegativeStat:
// keep existing L1 logic, then async-safe best-effort backend write/delete.
```

```cpp
// src/s3fs.cpp (startup wiring snippet)
MetadataBackendConfig mcfg;
mcfg.redis_uri = redis_meta_uri;
auto backend = CreateMetadataBackend(mcfg);
StatCache::getStatCacheData()->SetMetadataBackend(std::shared_ptr<MetadataBackend>(std::move(backend)));
```

- [ ] **Step 4: Run tests**

Run: `make -C src test_capacity_policy test_metadata_backend_factory test_curl_util test_page_list test_string_util`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/cache.h src/cache.cpp src/s3fs.cpp src/test_metadata_backend_factory.cpp
git commit -m "feat: add stat cache L2 backend integration with safe fallback"
```

---

### Task 6: Add Redis Capacity Counter Plumbing (TDD)

**Files:**
- Modify: `src/metadata_backend.h`
- Modify: `src/metadata_backend_null.cpp`
- Modify: `src/metadata_backend_redis.cpp`
- Modify: `src/s3fs.cpp`
- Test: `src/test_capacity_policy.cpp`

- [ ] **Step 1: Write failing contract test for used-bytes clamp**

```cpp
// append in src/test_capacity_policy.cpp
{
    CapacityResult r = ComputeCapacity(CapacityMode::Redis, 0, (1ULL << 40), (2ULL << 40), block);
    assert(r.free_bytes == 0);
}
```

- [ ] **Step 2: Run test**

Run: `make -C src test_capacity_policy && ./src/test_capacity_policy`  
Expected: PASS after clamp behavior is preserved.

- [ ] **Step 3: Implement counter read/write APIs and wire statfs**

```cpp
// src/metadata_backend.h
class MetadataBackend
{
public:
    virtual ~MetadataBackend() = default;
    virtual std::string Name() const = 0;
    virtual uint64_t GetUsedBytes() const = 0;
    virtual bool AddUsedBytesDelta(int64_t delta) = 0;
};
```

```cpp
// src/metadata_backend_null.cpp
uint64_t GetUsedBytes() const override { return 0; }
bool AddUsedBytesDelta(int64_t) override { return true; }
```

```cpp
// src/s3fs.cpp (examples)
static uint64_t GetCapacityUsedBytes()
{
    auto backend = StatCache::getStatCacheData()->GetMetadataBackend();
    if(!backend){ return 0; }
    return backend->GetUsedBytes();
}
```

- [ ] **Step 4: Run tests**

Run: `make -C src test_capacity_policy test_metadata_backend_factory`  
Expected: PASS.

Run: `make -C src test_curl_util test_page_list test_string_util`  
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/metadata_backend.h src/metadata_backend_null.cpp src/metadata_backend_redis.cpp src/s3fs.cpp src/test_capacity_policy.cpp
git commit -m "feat: add used-bytes capacity counters for redis capacity mode"
```

---

### Task 7: Docs + Man Page + End-To-End Verification

**Files:**
- Modify: `doc/man/s3fs.1.in`
- Modify: `README.md`

- [ ] **Step 1: Write failing docs consistency check**

```bash
rg -n "capacity_mode|redis_meta|bucket_size" doc/man/s3fs.1.in README.md
```

Expected before update: missing one or more new options/semantics.

- [ ] **Step 2: Update docs**

```text
doc/man/s3fs.1.in additions:
-o capacity_mode (legacy|redis)
-o redis_meta, redis_connect_timeout, redis_readwrite_timeout
In redis mode, bucket_size is total capacity source; default 1T when not explicitly set.
```

```text
README.md additions:
Short section "Redis metadata cache and dynamic capacity".
```

- [ ] **Step 3: Run verification**

Run: `make -C src test_capacity_policy test_metadata_backend_factory test_curl_util test_page_list test_string_util`  
Expected: PASS.

Run: `git diff --check`  
Expected: no whitespace errors.

- [ ] **Step 4: Commit**

```bash
git add doc/man/s3fs.1.in README.md
git commit -m "docs: document redis metadata mode and capacity_mode semantics"
```

---

## Final Verification Gate

- [ ] Run: `git log --oneline -n 8`  
Expected: task commits present in order.

- [ ] Run: `git status --short`  
Expected: clean working tree (excluding intentionally untracked local IDE files like `.idea/`).

- [ ] Run: `make -C src test_capacity_policy test_metadata_backend_factory test_curl_util test_page_list test_string_util`  
Expected: PASS.

---

## Self-Review Results

1. **Spec coverage:** Covered Redis L2 metadata, `capacity_mode=redis`, `bucket_size` as capacity source, default `1T`, optional hiredis, fallback behavior, docs, and test strategy.
2. **Placeholder scan:** Removed `TBD/TODO` style placeholders; each task has concrete files/commands.
3. **Type consistency:** `CapacityMode`, `ComputeCapacity`, `MetadataBackend`, and `CreateMetadataBackend` names are consistent across tasks.

