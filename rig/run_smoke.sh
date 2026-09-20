#!/bin/sh
# The rig's single entry point, for CI and for humans. Runs the compose smoke,
# then asserts that FreeSWITCH itself exited cleanly: smoke.sh ends by asking
# FreeSWITCH to shut down, so the container's exit code is a real shutdown
# result and not a signal from the teardown.
set -eu

RIG_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPOSE="docker compose -f $RIG_DIR/docker-compose.yml"
RIG_LOG_FILE=${RIG_LOG_FILE:-rig-logs.txt}
EXIT_TIMEOUT_SECONDS=${EXIT_TIMEOUT_SECONDS:-180}

# Prints the service's exit code, or nothing if it is still running when the
# timeout expires.
wait_for_exit() {
  cid=$($COMPOSE ps -aq "$1")
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

$COMPOSE up --build -d

smoke_code=$(wait_for_exit smoke)
fs_code=$(wait_for_exit freeswitch)

$COMPOSE logs --no-color > "$RIG_LOG_FILE" 2>&1 || true
$COMPOSE logs --no-color --no-log-prefix smoke 2>/dev/null || true
$COMPOSE down -v >/dev/null 2>&1 || true

echo "smoke exit code: ${smoke_code:-<still running>}"
echo "freeswitch exit code: ${fs_code:-<still running>}"
echo "full rig logs: $RIG_LOG_FILE"

[ "${smoke_code:-1}" -eq 0 ] || { echo "RIG FAIL: the smoke did not pass" >&2; exit 1; }
[ "${fs_code:-1}" -eq 0 ] ||
  { echo "RIG FAIL: FreeSWITCH exited ${fs_code:-<still running>}, expected 0" >&2; exit 1; }

echo "RIG PASS"
