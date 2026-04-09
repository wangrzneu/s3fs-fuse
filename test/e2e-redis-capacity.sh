#!/usr/bin/env bash
set -euo pipefail

# End-to-end validation for:
# 1) baseline s3fs behavior (legacy mode)
# 2) redis capacity mode defaults (1TiB when bucket_size is not set)
# 3) redis capacity mode explicit bucket_size
#
# Usage example:
#   BUCKET=my-bucket \
#   PASSWD_FILE=/home/ubuntu/.passwd-s3fs \
#   REDIS_URI=redis://127.0.0.1:6379/0 \
#   ./test/e2e-redis-capacity.sh

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
S3FS_BIN="${S3FS_BIN:-$ROOT_DIR/src/s3fs}"
MOUNT_BASE="${MOUNT_BASE:-/tmp/s3fs-e2e}"
PASSWD_FILE="${PASSWD_FILE:-}"
BUCKET="${BUCKET:-}"
REDIS_URI="${REDIS_URI:-redis://127.0.0.1:6379/0}"
S3_URL="${S3_URL:-}"
USE_PATH_REQUEST_STYLE="${USE_PATH_REQUEST_STYLE:-0}"
ENABLE_CURLDBG="${ENABLE_CURLDBG:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"
KEEP_MOUNT="${KEEP_MOUNT:-0}"
RUN_LEGACY="${RUN_LEGACY:-1}"
RUN_REDIS_DEFAULT="${RUN_REDIS_DEFAULT:-1}"
RUN_REDIS_EXPLICIT="${RUN_REDIS_EXPLICIT:-1}"
REDIS_EXPLICIT_BUCKET_SIZE="${REDIS_EXPLICIT_BUCKET_SIZE:-2TiB}"
RUN_COMPLEX_FS_OPS="${RUN_COMPLEX_FS_OPS:-1}"
TIMED_IO_DURATION_SEC="${TIMED_IO_DURATION_SEC:-600}"
TIMED_IO_TARGET="${TIMED_IO_TARGET:-redis-default}"
TIMED_IO_SMALL_FILE_BYTES="${TIMED_IO_SMALL_FILE_BYTES:-4096}"
TIMED_IO_LARGE_FILE_MB="${TIMED_IO_LARGE_FILE_MB:-8}"
TIMED_IO_PROGRESS_INTERVAL_SEC="${TIMED_IO_PROGRESS_INTERVAL_SEC:-60}"

EXPECTED_1T=$((1024 * 1024 * 1024 * 1024))
EXPECTED_2T=$((2 * 1024 * 1024 * 1024 * 1024))
NS_IN_MS=1000000
NS_IN_SEC=1000000000

declare -a PERF_STAGE_NAMES=()
declare -a PERF_STAGE_NS=()

log() {
  printf '[E2E] %s\n' "$*"
}

fail() {
  printf '[E2E][FAIL] %s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

run() {
  log "RUN: $*"
  "$@"
}

now_ns() {
  local now
  now="$(date +%s%N 2>/dev/null || true)"
  if [[ -z "$now" || "$now" == *N ]]; then
    now="$(( $(date +%s) * NS_IN_SEC ))"
  fi
  printf '%s' "$now"
}

ns_to_ms() {
  local ns="$1"
  printf '%d' "$(( ns / NS_IN_MS ))"
}

record_perf_ns() {
  local stage="$1"
  local elapsed_ns="$2"
  PERF_STAGE_NAMES+=("$stage")
  PERF_STAGE_NS+=("$elapsed_ns")
  log "perf stage=${stage} elapsed_ms=$(ns_to_ms "$elapsed_ns")"
}

measure_call() {
  local stage="$1"
  shift

  local begin_ns
  local end_ns
  begin_ns="$(now_ns)"
  if "$@"; then
    end_ns="$(now_ns)"
    record_perf_ns "$stage" "$((end_ns - begin_ns))"
    return 0
  fi

  local rc=$?
  end_ns="$(now_ns)"
  record_perf_ns "${stage}(failed)" "$((end_ns - begin_ns))"
  return "$rc"
}

print_perf_summary() {
  local count="${#PERF_STAGE_NS[@]}"
  if [[ "$count" -eq 0 ]]; then
    return
  fi

  local total_ns=0
  local i
  for i in "${!PERF_STAGE_NS[@]}"; do
    total_ns=$((total_ns + PERF_STAGE_NS[i]))
  done

  log "==== Perf Summary ===="
  log "perf total_stages=${count} total_ms=$(ns_to_ms "$total_ns")"

  local tmp_file
  tmp_file="$(mktemp)"
  for i in "${!PERF_STAGE_NS[@]}"; do
    printf '%s|%s\n' "${PERF_STAGE_NS[i]}" "${PERF_STAGE_NAMES[i]}" >> "$tmp_file"
  done

  local rank=0
  while IFS='|' read -r elapsed_ns stage; do
    rank=$((rank + 1))
    if [[ "$rank" -gt 12 ]]; then
      break
    fi
    log "perf top${rank} stage=${stage} elapsed_ms=$(ns_to_ms "$elapsed_ns")"
  done < <(sort -t'|' -nr -k1,1 "$tmp_file")
  rm -f "$tmp_file"
}

unmount_if_needed() {
  local mp="$1"
  if mountpoint -q "$mp"; then
    if command -v fusermount3 >/dev/null 2>&1; then
      run fusermount3 -u "$mp"
    elif command -v fusermount >/dev/null 2>&1; then
      run fusermount -u "$mp"
    else
      run umount "$mp"
    fi
  fi
}

cleanup_mount() {
  local mp="$1"
  if [[ "$KEEP_MOUNT" != "1" ]]; then
    unmount_if_needed "$mp" || true
    rmdir "$mp" >/dev/null 2>&1 || true
  fi
}

build_if_needed() {
  if [[ "$SKIP_BUILD" == "1" ]]; then
    return
  fi
  pushd "$ROOT_DIR" >/dev/null
  if [[ ! -x "$S3FS_BIN" ]]; then
    run ./autogen.sh
    run ./configure --with-hiredis
  fi
  run make -C src -j"$(nproc)"
  popd >/dev/null
}

common_mount_opts() {
  local opts
  opts="passwd_file=${PASSWD_FILE},use_cache=/tmp/s3fs-e2e-cache,dbglevel=info"
  if [[ "$ENABLE_CURLDBG" == "1" ]]; then
    opts+=",curldbg"
  fi
  if [[ -n "$S3_URL" ]]; then
    opts+=",url=${S3_URL}"
  fi
  if [[ "$USE_PATH_REQUEST_STYLE" == "1" ]]; then
    opts+=",use_path_request_style"
  fi
  printf '%s' "$opts"
}

read_df_size_used_avail() {
  local mp="$1"
  df -B1 "$mp" | awk 'NR==2 {print $2, $3, $4}'
}

assert_eq() {
  local expected="$1"
  local actual="$2"
  local message="$3"
  [[ "$expected" == "$actual" ]] || fail "${message}: expected=${expected}, actual=${actual}"
}

assert_ge() {
  local lhs="$1"
  local rhs="$2"
  local message="$3"
  (( lhs >= rhs )) || fail "${message}: lhs=${lhs}, rhs=${rhs}"
}

assert_gt() {
  local lhs="$1"
  local rhs="$2"
  local message="$3"
  (( lhs > rhs )) || fail "${message}: lhs=${lhs}, rhs=${rhs}"
}

assert_le() {
  local lhs="$1"
  local rhs="$2"
  local message="$3"
  (( lhs <= rhs )) || fail "${message}: lhs=${lhs}, rhs=${rhs}"
}

read_redis_used_bytes() {
  local key="$1"
  local value
  value="$(redis-cli --raw GET "$key" 2>/dev/null || true)"
  if [[ -z "$value" ]]; then
    value=0
  fi
  printf '%s' "$value"
}

assert_redis_write_path_counter() {
  local mp="$1"
  local key="$2"
  local case_name="$3"

  local begin_ns
  local end_ns

  run redis-cli DEL "$key"
  local baseline
  baseline="$(read_redis_used_bytes "$key")"

  begin_ns="$(now_ns)"
  run dd if=/dev/zero of="$mp/e2e-counter.bin" bs=1M count=4 status=none
  sleep 2
  end_ns="$(now_ns)"
  record_perf_ns "${case_name}.counter_write" "$((end_ns - begin_ns))"
  local after_write
  after_write="$(read_redis_used_bytes "$key")"
  assert_gt "$after_write" "$baseline" "redis used-bytes should increase after write"

  begin_ns="$(now_ns)"
  run truncate -s 1M "$mp/e2e-counter.bin"
  sleep 2
  end_ns="$(now_ns)"
  record_perf_ns "${case_name}.counter_truncate" "$((end_ns - begin_ns))"
  local after_truncate
  after_truncate="$(read_redis_used_bytes "$key")"
  assert_le "$after_truncate" "$after_write" "redis used-bytes should not increase after truncate shrink"

  begin_ns="$(now_ns)"
  run rm -f "$mp/e2e-counter.bin"
  sleep 2
  end_ns="$(now_ns)"
  record_perf_ns "${case_name}.counter_delete" "$((end_ns - begin_ns))"
  local after_delete
  after_delete="$(read_redis_used_bytes "$key")"
  assert_eq "$baseline" "$after_delete" "redis used-bytes should return to baseline after delete"

  log "redis counter bytes: baseline=${baseline} after_write=${after_write} after_truncate=${after_truncate} after_delete=${after_delete}"
}

assert_basic_io() {
  local mp="$1"
  run mkdir -p "$mp/e2e-dir"
  run sh -c "echo 'hello-s3fs-e2e' > '$mp/e2e-dir/a.txt'"
  local out
  out="$(cat "$mp/e2e-dir/a.txt")"
  assert_eq "hello-s3fs-e2e" "$out" "content mismatch"
  run mv "$mp/e2e-dir/a.txt" "$mp/e2e-dir/b.txt"
  run rm -f "$mp/e2e-dir/b.txt"
  run rmdir "$mp/e2e-dir"
}

assert_complex_fs_ops() {
  local mp="$1"
  local root="$mp/e2e-complex"
  local case_name="$2"
  local begin_ns
  local end_ns

  log "complex fs ops: start"

  begin_ns="$(now_ns)"
  mkdir -p "$root/dir-a/sub-1" "$root/dir-b"
  printf 'v1\n' > "$root/dir-a/sub-1/file-a.txt"
  printf 'line-1\nline-2\n' > "$root/dir-a/sub-1/file-b.txt"
  dd if=/dev/zero of="$root/dir-a/sub-1/blob.bin" bs=1K count=128 status=none

  local first_line
  first_line="$(head -n1 "$root/dir-a/sub-1/file-a.txt")"
  assert_eq "v1" "$first_line" "complex ops initial content mismatch"
  end_ns="$(now_ns)"
  record_perf_ns "${case_name}.complex_create" "$((end_ns - begin_ns))"

  begin_ns="$(now_ns)"
  printf 'v2\n' > "$root/dir-a/sub-1/file-a.txt"
  printf 'tail-marker\n' >> "$root/dir-a/sub-1/file-a.txt"
  truncate -s 2048 "$root/dir-a/sub-1/blob.bin"

  local tail_line
  tail_line="$(tail -n1 "$root/dir-a/sub-1/file-a.txt")"
  assert_eq "tail-marker" "$tail_line" "complex ops append mismatch"
  end_ns="$(now_ns)"
  record_perf_ns "${case_name}.complex_modify" "$((end_ns - begin_ns))"

  begin_ns="$(now_ns)"
  mv "$root/dir-a/sub-1/file-a.txt" "$root/dir-b/file-a-renamed.txt"
  mv "$root/dir-a/sub-1" "$root/dir-a/sub-1-renamed"
  mkdir -p "$root/dir-a/sub-1-renamed/new-child"
  printf 'child-data\n' > "$root/dir-a/sub-1-renamed/new-child/file-c.txt"

  [[ -f "$root/dir-b/file-a-renamed.txt" ]] || fail "complex ops rename file failed"
  [[ -d "$root/dir-a/sub-1-renamed" ]] || fail "complex ops rename directory failed"
  end_ns="$(now_ns)"
  record_perf_ns "${case_name}.complex_rename" "$((end_ns - begin_ns))"

  begin_ns="$(now_ns)"
  rm -f "$root/dir-b/file-a-renamed.txt"
  rm -f "$root/dir-a/sub-1-renamed/file-b.txt"
  rm -f "$root/dir-a/sub-1-renamed/blob.bin"
  rm -f "$root/dir-a/sub-1-renamed/new-child/file-c.txt"
  rmdir "$root/dir-a/sub-1-renamed/new-child"
  rmdir "$root/dir-a/sub-1-renamed"
  rmdir "$root/dir-a"
  rmdir "$root/dir-b"
  rmdir "$root"
  end_ns="$(now_ns)"
  record_perf_ns "${case_name}.complex_cleanup" "$((end_ns - begin_ns))"

  log "complex fs ops: done"
}

should_run_timed_io_for_case() {
  local case_name="$1"
  [[ "$TIMED_IO_DURATION_SEC" -gt 0 ]] || return 1
  if [[ "$TIMED_IO_TARGET" == "all" ]]; then
    return 0
  fi
  [[ "$TIMED_IO_TARGET" == "$case_name" ]]
}

assert_timed_small_large_io() {
  local mp="$1"
  local case_name="$2"
  local root="$mp/e2e-timed-io"
  local start_ns
  start_ns="$(now_ns)"
  local start_ts
  start_ts="$(date +%s)"
  local end_ts=$((start_ts + TIMED_IO_DURATION_SEC))
  local next_progress_ts=$((start_ts + TIMED_IO_PROGRESS_INTERVAL_SEC))
  local iter=0
  local small_index=0
  local large_index=0
  local large_bytes=$((TIMED_IO_LARGE_FILE_MB * 1024 * 1024))
  local half_large_bytes=$((large_bytes / 2))
  local phase_small_write_ns=0
  local phase_small_read_ns=0
  local phase_large_write_ns=0
  local phase_large_read_ns=0
  local phase_large_truncate_ns=0
  local phase_large_rewrite_ns=0

  log "timed io (${case_name}): start duration=${TIMED_IO_DURATION_SEC}s small=${TIMED_IO_SMALL_FILE_BYTES}B large=${TIMED_IO_LARGE_FILE_MB}MiB"

  mkdir -p "$root/small" "$root/large"

  while [[ "$(date +%s)" -lt "$end_ts" ]]; do
    iter=$((iter + 1))
    small_index=$((iter % 64))
    large_index=$((iter % 16))

    local small_file="$root/small/small-${small_index}.dat"
    local large_file="$root/large/large-${large_index}.bin"

    local t0
    local t1
    t0="$(now_ns)"
    dd if=/dev/zero of="$small_file" bs="$TIMED_IO_SMALL_FILE_BYTES" count=1 status=none
    t1="$(now_ns)"
    phase_small_write_ns=$((phase_small_write_ns + t1 - t0))

    t0="$(now_ns)"
    dd if="$small_file" of=/dev/null bs="$TIMED_IO_SMALL_FILE_BYTES" status=none
    t1="$(now_ns)"
    phase_small_read_ns=$((phase_small_read_ns + t1 - t0))
    printf 'iter-%s\n' "$iter" >> "$small_file"

    t0="$(now_ns)"
    dd if=/dev/zero of="$large_file" bs=1M count="$TIMED_IO_LARGE_FILE_MB" status=none
    t1="$(now_ns)"
    phase_large_write_ns=$((phase_large_write_ns + t1 - t0))

    t0="$(now_ns)"
    dd if="$large_file" of=/dev/null bs=1M status=none
    t1="$(now_ns)"
    phase_large_read_ns=$((phase_large_read_ns + t1 - t0))

    t0="$(now_ns)"
    truncate -s "$half_large_bytes" "$large_file"
    t1="$(now_ns)"
    phase_large_truncate_ns=$((phase_large_truncate_ns + t1 - t0))

    t0="$(now_ns)"
    dd if="$large_file" of=/dev/null bs=1M status=none
    t1="$(now_ns)"
    phase_large_read_ns=$((phase_large_read_ns + t1 - t0))

    t0="$(now_ns)"
    dd if=/dev/zero of="$large_file" bs=1M seek="$TIMED_IO_LARGE_FILE_MB" count=1 conv=notrunc status=none
    t1="$(now_ns)"
    phase_large_rewrite_ns=$((phase_large_rewrite_ns + t1 - t0))

    t0="$(now_ns)"
    dd if="$large_file" of=/dev/null bs=1M status=none
    t1="$(now_ns)"
    phase_large_read_ns=$((phase_large_read_ns + t1 - t0))

    if (( iter % 7 == 0 )); then
      rm -f "$small_file"
    fi
    if (( iter % 5 == 0 )); then
      rm -f "$large_file"
    fi

    local now_ts
    now_ts="$(date +%s)"
    if [[ "$now_ts" -ge "$next_progress_ts" ]]; then
      log "timed io (${case_name}): progress elapsed=$((now_ts - start_ts))s iter=${iter}"
      next_progress_ts=$((next_progress_ts + TIMED_IO_PROGRESS_INTERVAL_SEC))
    fi
  done

  rm -f "$root/small/"* "$root/large/"* 2>/dev/null || true
  rmdir "$root/small" "$root/large" "$root" 2>/dev/null || true

  local total_elapsed_ns
  total_elapsed_ns="$(( $(now_ns) - start_ns ))"
  local avg_iter_ms=0
  if [[ "$iter" -gt 0 ]]; then
    avg_iter_ms=$(( (total_elapsed_ns / NS_IN_MS) / iter ))
  fi

  record_perf_ns "${case_name}.timed_io_total" "$total_elapsed_ns"
  record_perf_ns "${case_name}.timed_io_small_write_total" "$phase_small_write_ns"
  record_perf_ns "${case_name}.timed_io_small_read_total" "$phase_small_read_ns"
  record_perf_ns "${case_name}.timed_io_large_write_total" "$phase_large_write_ns"
  record_perf_ns "${case_name}.timed_io_large_read_total" "$phase_large_read_ns"
  record_perf_ns "${case_name}.timed_io_large_truncate_total" "$phase_large_truncate_ns"
  record_perf_ns "${case_name}.timed_io_large_rewrite_total" "$phase_large_rewrite_ns"

  log "timed io (${case_name}): done iterations=${iter}"
  log "timed io (${case_name}): avg_iter_ms=${avg_iter_ms}"
}

mount_and_validate_legacy() {
  local mp="$MOUNT_BASE/legacy"
  mkdir -p "$mp"
  trap 'cleanup_mount "'"$mp"'"' RETURN

  local opts
  opts="$(common_mount_opts)"
  local mount_begin_ns
  mount_begin_ns="$(now_ns)"
  run "$S3FS_BIN" "$BUCKET" "$mp" -o "$opts" -f &
  local pid=$!
  sleep 4
  mountpoint -q "$mp" || fail "legacy mount failed"
  local mount_end_ns
  mount_end_ns="$(now_ns)"
  record_perf_ns "legacy.mount_ready" "$((mount_end_ns - mount_begin_ns))"
  log "legacy mounted pid=$pid"

  measure_call "legacy.basic_io" assert_basic_io "$mp"
  if [[ "$RUN_COMPLEX_FS_OPS" == "1" ]]; then
    measure_call "legacy.complex_fs_ops" assert_complex_fs_ops "$mp" "legacy"
  fi
  if should_run_timed_io_for_case "legacy"; then
    measure_call "legacy.timed_small_large_io" assert_timed_small_large_io "$mp" "legacy"
  fi

  local statfs_begin_ns
  local statfs_end_ns
  statfs_begin_ns="$(now_ns)"
  read -r size used avail <<<"$(read_df_size_used_avail "$mp")"
  statfs_end_ns="$(now_ns)"
  record_perf_ns "legacy.statfs" "$((statfs_end_ns - statfs_begin_ns))"
  log "legacy df bytes: size=${size} used=${used} avail=${avail}"
  assert_ge "$size" "$avail" "legacy size must be >= avail"

  local unmount_begin_ns
  local unmount_end_ns
  unmount_begin_ns="$(now_ns)"
  cleanup_mount "$mp"
  kill "$pid" >/dev/null 2>&1 || true
  unmount_end_ns="$(now_ns)"
  record_perf_ns "legacy.unmount_cleanup" "$((unmount_end_ns - unmount_begin_ns))"
}

mount_and_validate_redis_default() {
  local mp="$MOUNT_BASE/redis-default"
  mkdir -p "$mp"
  trap 'cleanup_mount "'"$mp"'"' RETURN

  local opts
  opts="$(common_mount_opts)"
  opts+=",capacity_mode=redis,redis_meta=${REDIS_URI}"
  local mount_begin_ns
  mount_begin_ns="$(now_ns)"
  run "$S3FS_BIN" "$BUCKET" "$mp" -o "$opts" -f &
  local pid=$!
  sleep 4
  mountpoint -q "$mp" || fail "redis default mount failed"
  local mount_end_ns
  mount_end_ns="$(now_ns)"
  record_perf_ns "redis-default.mount_ready" "$((mount_end_ns - mount_begin_ns))"
  log "redis(default) mounted pid=$pid"

  measure_call "redis-default.basic_io" assert_basic_io "$mp"
  if [[ "$RUN_COMPLEX_FS_OPS" == "1" ]]; then
    measure_call "redis-default.complex_fs_ops" assert_complex_fs_ops "$mp" "redis-default"
  fi
  measure_call "redis-default.redis_counter" assert_redis_write_path_counter "$mp" "s3fs:capacity:used_bytes:${BUCKET}" "redis-default"
  if should_run_timed_io_for_case "redis-default"; then
    measure_call "redis-default.timed_small_large_io" assert_timed_small_large_io "$mp" "redis-default"
  fi

  local statfs_begin_ns
  local statfs_end_ns
  statfs_begin_ns="$(now_ns)"
  read -r size used avail <<<"$(read_df_size_used_avail "$mp")"
  statfs_end_ns="$(now_ns)"
  record_perf_ns "redis-default.statfs" "$((statfs_end_ns - statfs_begin_ns))"
  log "redis(default) df bytes: size=${size} used=${used} avail=${avail}"
  assert_eq "$EXPECTED_1T" "$size" "redis default bucket_size should be 1TiB"
  assert_ge "$size" "$avail" "redis default size must be >= avail"

  local unmount_begin_ns
  local unmount_end_ns
  unmount_begin_ns="$(now_ns)"
  cleanup_mount "$mp"
  kill "$pid" >/dev/null 2>&1 || true
  unmount_end_ns="$(now_ns)"
  record_perf_ns "redis-default.unmount_cleanup" "$((unmount_end_ns - unmount_begin_ns))"
}

mount_and_validate_redis_explicit() {
  local mp="$MOUNT_BASE/redis-explicit"
  mkdir -p "$mp"
  trap 'cleanup_mount "'"$mp"'"' RETURN

  local opts
  opts="$(common_mount_opts)"
  opts+=",capacity_mode=redis,redis_meta=${REDIS_URI},bucket_size=${REDIS_EXPLICIT_BUCKET_SIZE}"
  local mount_begin_ns
  mount_begin_ns="$(now_ns)"
  run "$S3FS_BIN" "$BUCKET" "$mp" -o "$opts" -f &
  local pid=$!
  sleep 4
  mountpoint -q "$mp" || fail "redis explicit mount failed"
  local mount_end_ns
  mount_end_ns="$(now_ns)"
  record_perf_ns "redis-explicit.mount_ready" "$((mount_end_ns - mount_begin_ns))"
  log "redis(explicit) mounted pid=$pid"

  measure_call "redis-explicit.basic_io" assert_basic_io "$mp"
  if [[ "$RUN_COMPLEX_FS_OPS" == "1" ]]; then
    measure_call "redis-explicit.complex_fs_ops" assert_complex_fs_ops "$mp" "redis-explicit"
  fi
  measure_call "redis-explicit.redis_counter" assert_redis_write_path_counter "$mp" "s3fs:capacity:used_bytes:${BUCKET}" "redis-explicit"
  if should_run_timed_io_for_case "redis-explicit"; then
    measure_call "redis-explicit.timed_small_large_io" assert_timed_small_large_io "$mp" "redis-explicit"
  fi

  local statfs_begin_ns
  local statfs_end_ns
  statfs_begin_ns="$(now_ns)"
  read -r size used avail <<<"$(read_df_size_used_avail "$mp")"
  statfs_end_ns="$(now_ns)"
  record_perf_ns "redis-explicit.statfs" "$((statfs_end_ns - statfs_begin_ns))"
  log "redis(explicit) df bytes: size=${size} used=${used} avail=${avail}"
  if [[ "$REDIS_EXPLICIT_BUCKET_SIZE" == "2TiB" ]]; then
    assert_eq "$EXPECTED_2T" "$size" "redis explicit bucket_size should be 2TiB"
  fi
  assert_ge "$size" "$avail" "redis explicit size must be >= avail"

  local unmount_begin_ns
  local unmount_end_ns
  unmount_begin_ns="$(now_ns)"
  cleanup_mount "$mp"
  kill "$pid" >/dev/null 2>&1 || true
  unmount_end_ns="$(now_ns)"
  record_perf_ns "redis-explicit.unmount_cleanup" "$((unmount_end_ns - unmount_begin_ns))"
}

main() {
  need_cmd mountpoint
  need_cmd df
  need_cmd awk
  need_cmd sort
  need_cmd mktemp
  need_cmd dd
  need_cmd truncate
  if [[ "$RUN_REDIS_DEFAULT" == "1" || "$RUN_REDIS_EXPLICIT" == "1" ]]; then
    need_cmd redis-cli
  fi

  [[ -n "$BUCKET" ]] || fail "BUCKET is required"
  [[ -n "$PASSWD_FILE" ]] || fail "PASSWD_FILE is required"
  [[ -x "$S3FS_BIN" || "$SKIP_BUILD" == "0" ]] || fail "s3fs binary not found: $S3FS_BIN"

  measure_call "build_if_needed" build_if_needed

  mkdir -p "$MOUNT_BASE"
  mkdir -p /tmp/s3fs-e2e-cache

  if [[ "$RUN_LEGACY" == "1" ]]; then
    log "==== Case 1: legacy mode ===="
    measure_call "case.legacy.total" mount_and_validate_legacy
  fi

  if [[ "$RUN_REDIS_DEFAULT" == "1" ]]; then
    log "==== Case 2: redis mode with default bucket_size (1TiB) ===="
    measure_call "case.redis-default.total" mount_and_validate_redis_default
  fi

  if [[ "$RUN_REDIS_EXPLICIT" == "1" ]]; then
    log "==== Case 3: redis mode with explicit bucket_size=${REDIS_EXPLICIT_BUCKET_SIZE} ===="
    measure_call "case.redis-explicit.total" mount_and_validate_redis_explicit
  fi

  print_perf_summary
  log "E2E completed"
}

main "$@"
