#!/bin/sh
# Fork a call, assert the mock server saw a well-formed stream, tear down, and
# assert every resource went back. Exits non-zero on the first violation so CI
# fails loudly rather than logging a warning nobody reads.
set -eu

FS_CLI=${FS_CLI:-fs_cli}
MOCK_WS_URL=${MOCK_WS_URL:-ws://mock-ws:9099/}
REPORT=${MOCK_WS_REPORT:-/tmp/mock-report.json}
CALLS=${CALLS:-1}

cli() { "$FS_CLI" -H 127.0.0.1 -x "$1"; }

fail() {
  echo "SMOKE FAIL: $1" >&2
  exit 1
}

echo "waiting for the module to load"
i=0
while [ "$i" -lt 30 ]; do
  if cli "audio_fork status" >/dev/null 2>&1; then break; fi
  i=$((i + 1))
  sleep 1
done
cli "audio_fork status" >/dev/null 2>&1 || fail "module never answered audio_fork status"

uuids=""
n=0
while [ "$n" -lt "$CALLS" ]; do
  uuid=$(cli "create_uuid" | tr -d '\r\n ')
  cli "bgapi originate {origination_uuid=$uuid}loopback/rig-tone &park" >/dev/null
  uuids="$uuids $uuid"
  n=$((n + 1))
done
sleep 3

for uuid in $uuids; do
  out=$(cli "uuid_audio_fork $uuid start $MOCK_WS_URL mono 16000 {\"rig\":true}")
  case "$out" in
    *"+OK"*) ;;
    *) fail "start rejected for $uuid: $out" ;;
  esac
done

echo "streaming for 5s"
sleep 5

status=$(cli "audio_fork status")
echo "status: $status"
case "$status" in
  *'"forks":0'*) fail "no forks active while streaming" ;;
esac
case "$status" in
  *'"sent_bytes":0'*) fail "no audio was sent" ;;
esac

for uuid in $uuids; do
  cli "uuid_audio_fork $uuid stop" >/dev/null || true
  cli "uuid_kill $uuid" >/dev/null || true
done
sleep 3

final=$(cli "audio_fork status")
echo "final: $final"
case "$final" in
  *'"forks":0'*) ;;
  *) fail "forks still active after teardown: $final" ;;
esac
case "$final" in
  *'"pool_leased_slabs":0'*) ;;
  *) fail "slabs still leased after teardown: $final" ;;
esac

if [ -f "$REPORT" ]; then
  echo "mock report: $(cat "$REPORT")"
  grep -q '"hello_count": 0' "$REPORT" && fail "server never received hello"
  grep -q '"audio_bytes": 0' "$REPORT" && fail "server never received audio"
  grep -q '"protocol_errors": \[\]' "$REPORT" || fail "server recorded protocol errors"
fi

echo "SMOKE PASS"
