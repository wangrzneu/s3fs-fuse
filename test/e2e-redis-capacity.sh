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
SKIP_BUILD="${SKIP_BUILD:-0}"
KEEP_MOUNT="${KEEP_MOUNT:-0}"
RUN_LEGACY="${RUN_LEGACY:-1}"
RUN_REDIS_DEFAULT="${RUN_REDIS_DEFAULT:-1}"
RUN_REDIS_EXPLICIT="${RUN_REDIS_EXPLICIT:-1}"
REDIS_EXPLICIT_BUCKET_SIZE="${REDIS_EXPLICIT_BUCKET_SIZE:-2TiB}"

EXPECTED_1T=$((1024 * 1024 * 1024 * 1024))
EXPECTED_2T=$((2 * 1024 * 1024 * 1024 * 1024))

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
  opts="passwd_file=${PASSWD_FILE},use_cache=/tmp/s3fs-e2e-cache,dbglevel=info,curldbg"
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

mount_and_validate_legacy() {
  local mp="$MOUNT_BASE/legacy"
  mkdir -p "$mp"
  trap 'cleanup_mount "'"$mp"'"' RETURN

  local opts
  opts="$(common_mount_opts)"
  run "$S3FS_BIN" "$BUCKET" "$mp" -o "$opts" -f &
  local pid=$!
  sleep 4
  mountpoint -q "$mp" || fail "legacy mount failed"
  log "legacy mounted pid=$pid"

  assert_basic_io "$mp"

  read -r size used avail <<<"$(read_df_size_used_avail "$mp")"
  log "legacy df bytes: size=${size} used=${used} avail=${avail}"
  assert_ge "$size" "$avail" "legacy size must be >= avail"

  cleanup_mount "$mp"
  kill "$pid" >/dev/null 2>&1 || true
}

mount_and_validate_redis_default() {
  local mp="$MOUNT_BASE/redis-default"
  mkdir -p "$mp"
  trap 'cleanup_mount "'"$mp"'"' RETURN

  local opts
  opts="$(common_mount_opts)"
  opts+=",capacity_mode=redis,redis_meta=${REDIS_URI}"
  run "$S3FS_BIN" "$BUCKET" "$mp" -o "$opts" -f &
  local pid=$!
  sleep 4
  mountpoint -q "$mp" || fail "redis default mount failed"
  log "redis(default) mounted pid=$pid"

  assert_basic_io "$mp"

  read -r size used avail <<<"$(read_df_size_used_avail "$mp")"
  log "redis(default) df bytes: size=${size} used=${used} avail=${avail}"
  assert_eq "$EXPECTED_1T" "$size" "redis default bucket_size should be 1TiB"
  assert_ge "$size" "$avail" "redis default size must be >= avail"

  cleanup_mount "$mp"
  kill "$pid" >/dev/null 2>&1 || true
}

mount_and_validate_redis_explicit() {
  local mp="$MOUNT_BASE/redis-explicit"
  mkdir -p "$mp"
  trap 'cleanup_mount "'"$mp"'"' RETURN

  local opts
  opts="$(common_mount_opts)"
  opts+=",capacity_mode=redis,redis_meta=${REDIS_URI},bucket_size=${REDIS_EXPLICIT_BUCKET_SIZE}"
  run "$S3FS_BIN" "$BUCKET" "$mp" -o "$opts" -f &
  local pid=$!
  sleep 4
  mountpoint -q "$mp" || fail "redis explicit mount failed"
  log "redis(explicit) mounted pid=$pid"

  assert_basic_io "$mp"

  read -r size used avail <<<"$(read_df_size_used_avail "$mp")"
  log "redis(explicit) df bytes: size=${size} used=${used} avail=${avail}"
  if [[ "$REDIS_EXPLICIT_BUCKET_SIZE" == "2TiB" ]]; then
    assert_eq "$EXPECTED_2T" "$size" "redis explicit bucket_size should be 2TiB"
  fi
  assert_ge "$size" "$avail" "redis explicit size must be >= avail"

  cleanup_mount "$mp"
  kill "$pid" >/dev/null 2>&1 || true
}

main() {
  need_cmd mountpoint
  need_cmd df
  need_cmd awk

  [[ -n "$BUCKET" ]] || fail "BUCKET is required"
  [[ -n "$PASSWD_FILE" ]] || fail "PASSWD_FILE is required"
  [[ -x "$S3FS_BIN" || "$SKIP_BUILD" == "0" ]] || fail "s3fs binary not found: $S3FS_BIN"

  build_if_needed

  mkdir -p "$MOUNT_BASE"
  mkdir -p /tmp/s3fs-e2e-cache

  if [[ "$RUN_LEGACY" == "1" ]]; then
    log "==== Case 1: legacy mode ===="
    mount_and_validate_legacy
  fi

  if [[ "$RUN_REDIS_DEFAULT" == "1" ]]; then
    log "==== Case 2: redis mode with default bucket_size (1TiB) ===="
    mount_and_validate_redis_default
  fi

  if [[ "$RUN_REDIS_EXPLICIT" == "1" ]]; then
    log "==== Case 3: redis mode with explicit bucket_size=${REDIS_EXPLICIT_BUCKET_SIZE} ===="
    mount_and_validate_redis_explicit
  fi

  log "E2E completed"
  log "Note: current implementation reads used bytes from backend, but write-path counter updates are not fully wired yet."
}

main "$@"
