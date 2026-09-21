#!/bin/sh
# Shared by run_load.sh, run_soak.sh and rig/chaos/run_chaos.sh: compose
# plumbing, FreeSWITCH sampling and the leak assertions all three end with.

RIG_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPO_DIR=$(git -C "$RIG_DIR" rev-parse --show-toplevel)
# One project and one image tag per worktree, as run_smoke.sh does: several
# checkouts on one host would otherwise share containers.
COMPOSE_PROJECT_NAME=${COMPOSE_PROJECT_NAME:-afrig-$(basename "$REPO_DIR")}
AUDIOFORK_IMAGE=${AUDIOFORK_IMAGE:-$COMPOSE_PROJECT_NAME-freeswitch}
SIPP_IMAGE=${SIPP_IMAGE:-$COMPOSE_PROJECT_NAME-sipp}
export COMPOSE_PROJECT_NAME AUDIOFORK_IMAGE SIPP_IMAGE

LOG_DIR=${LOG_DIR:-$REPO_DIR/rig-logs}
EXIT_TIMEOUT_SECONDS=${EXIT_TIMEOUT_SECONDS:-180}

compose() {
  docker compose -f "$RIG_DIR/docker-compose.yml" -f "$RIG_DIR/docker-compose.load.yml" \
    ${EXTRA_COMPOSE_FILE:+-f "$EXTRA_COMPOSE_FILE"} "$@"
}

fs() {
  compose exec -T freeswitch fs_cli -H 127.0.0.1 -x "$1"
}

status_json() {
  fs "audio_fork status"
}

# Reads one flat integer out of the status or report JSON. Nested values
# (shard_load) are arrays and are not addressed this way.
jnum() {
  printf '%s' "$2" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
value = doc.get(sys.argv[1])
sys.stdout.write("" if isinstance(value, (dict, list, type(None))) else str(value))
' "$1"
}

fail() {
  echo "RIG FAIL: $1" >&2
  exit 1
}

# RSS in kB and open descriptors of the FreeSWITCH process, read inside the
# container: the host has no view of them on Docker Desktop.
fs_rss_kb() {
  compose exec -T freeswitch sh -c \
    'pid=$(pgrep -x freeswitch | head -1); awk "/VmRSS/ { print \$2 }" /proc/$pid/status' |
    tr -d '\r'
}

fs_fd_count() {
  compose exec -T freeswitch sh -c \
    'pid=$(pgrep -x freeswitch | head -1); ls /proc/$pid/fd | wc -l' | tr -d '\r'
}

# The sampler writes into the report volume so the CSV survives the host's exec
# latency; fetch_samples copies it out once, at the end.
start_sampler() {
  compose exec -d freeswitch sh /srv/load/sampler.sh "/shared/$1" "$2"
}

fetch_samples() {
  compose exec -T freeswitch cat "/shared/$1" > "$LOG_DIR/$1" 2>/dev/null ||
    echo "no samples at /shared/$1" >&2
}

# Wall clock, not poll count: one poll costs a compose exec, which is seconds on
# a loaded Docker Desktop, so counting polls times the wait out far too early.
wait_for_idle() {
  deadline=$(( $(date +%s) + ${IDLE_TIMEOUT_SECONDS:-300} ))
  while [ "$(date +%s)" -lt "$deadline" ]; do
    [ "$(jnum forks "$(status_json)")" = "0" ] && return 0
    sleep 2
  done
  fail "forks never returned to 0 within ${IDLE_TIMEOUT_SECONDS:-300}s"
}

# busybox wget is the only HTTP client in the FreeSWITCH image and cannot send
# DELETE, so toxics are cleared with toxiproxy's POST /reset rather than by
# deleting them one at a time.
toxi() {
  compose exec -T freeswitch wget -q -O - -T 15 \
    --header='Content-Type: application/json' --post-data="${2:-}" \
    "http://toxiproxy:8474$1"
}

wait_for_module() {
  i=0
  while [ "$i" -lt 60 ]; do
    if status_json >/dev/null 2>&1; then return 0; fi
    i=$((i + 1))
    sleep 2
  done
  fail "the module never answered audio_fork status"
}

# Prints the service's exit code, or nothing if it is still running when the
# timeout expires.
wait_for_exit() {
  cid=$(compose ps -aq "$1")
  [ -n "$cid" ] || return 0
  i=0
  while [ "$i" -lt "$EXIT_TIMEOUT_SECONDS" ]; do
    if [ "$(docker inspect -f '{{.State.Running}}' "$cid")" = "false" ]; then
      docker inspect -f '{{.State.ExitCode}}' "$cid"
      return 0
    fi
    i=$((i + 1))
    sleep 1
  done
}

# Asks FreeSWITCH to shut down and asserts it exited cleanly, so the musl
# WRITE_REPLACE shutdown fault (DESIGN.md §13) cannot regress unnoticed.
shutdown_and_assert_exit() {
  fs "fsctl shutdown" >/dev/null 2>&1 || true
  fs_code=$(wait_for_exit freeswitch)
  echo "freeswitch exit code: ${fs_code:-<still running>}"
  [ "${fs_code:-1}" -eq 0 ] ||
    fail "FreeSWITCH exited ${fs_code:-<still running>}, expected 0"
}

collect_logs() {
  mkdir -p "$LOG_DIR"
  compose logs --no-color > "$LOG_DIR/$1" 2>&1 || true
}
