# S3FS Redis Capacity 1h Test Report

Date: 2026-04-09  
Server: `128.1.40.153` (`ubuntu`)  
Workspace: `/home/ubuntu/s3fs-fuse-e2e`  
Log file: `/tmp/e2e-capacity-1h.log`

## Test Goal

Validate capacity calculation correctness over long-duration IO (`1 hour`) in `capacity_mode=redis`.

## Command

```bash
RUN_LEGACY=0 \
RUN_REDIS_EXPLICIT=0 \
SKIP_BUILD=0 \
ENABLE_CURLDBG=0 \
RUN_COMPLEX_FS_OPS=0 \
BUCKET=s3fstest \
PASSWD_FILE=/home/ubuntu/.passwd-s3fs \
REDIS_URI=redis://127.0.0.1:6379/0 \
S3_URL=http://internal.s3-sg.ufileos.com \
USE_PATH_REQUEST_STYLE=1 \
TIMED_IO_DURATION_SEC=3600 \
TIMED_IO_TARGET=redis-default \
TIMED_IO_PROGRESS_INTERVAL_SEC=300 \
./test/e2e-redis-capacity.sh
```

## Acceptance Criteria

1. Redis used-bytes counter must increase on write, decrease on truncate shrink, and return to baseline after delete.
2. After 1-hour timed IO, `df` capacity should remain consistent with redis capacity policy (`1TiB` default in redis mode).
3. Test exits without `[E2E][FAIL]`.

## Result

Status: **PASS**

- Counter correctness:
  - `baseline=0`
  - `after_write=4194304`
  - `after_truncate=1048576`
  - `after_delete=0`
- Timed IO duration: `3600s`, iterations: `4381`
- Final `df`:
  - `size=1099511627776`
  - `used=0`
  - `avail=1099511627776`
- End marker: `[E2E] E2E completed`
- No `[E2E][FAIL]` found.

## Performance Snapshot (from script timing summary)

- `case.redis-default.total`: `3616532 ms`
- `redis-default.timed_io_total`: `3605836 ms`
- `redis-default.timed_io_large_read_total`: `1024983 ms`
- `redis-default.timed_io_large_write_total`: `876265 ms`
- `redis-default.timed_io_large_rewrite_total`: `798490 ms`
- `redis-default.timed_io_large_truncate_total`: `528920 ms`
- `redis-default.timed_io_small_write_total`: `128051 ms`
- `redis-default.timed_io_small_read_total`: `77543 ms`

## Optimization Direction (based on measured bottlenecks)

1. Reduce large-file read amplification in timed IO path.
2. Reduce rewrite frequency/size for large files.
3. Optimize truncate path for large object update.
4. Keep `curldbg` disabled during perf baselines to avoid log overhead distortion.
