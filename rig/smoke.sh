#!/bin/sh
# Fork a call, assert the mock server saw a well-formed stream, tear down, and
# assert every resource went back. Exits non-zero on the first violation so CI
# fails loudly rather than logging a warning nobody reads.
set -eu

FS_CLI=${FS_CLI:-fs_cli}
MOCK_WS_URL=${MOCK_WS_URL:-ws://mock-ws:9099/}
REPORT=${MOCK_WS_REPORT:-/tmp/mock-report.json}
CALLS=${CALLS:-1}
STREAM_SECONDS=${STREAM_SECONDS:-5}

# 16 kHz mono L16 is 32000 bytes per second on the wire.
WIRE_BYTES_PER_SECOND=32000
TOLERANCE_PERCENT=30
RATE_MIN=$((WIRE_BYTES_PER_SECOND * (100 - TOLERANCE_PERCENT) / 100))
RATE_MAX=$((WIRE_BYTES_PER_SECOND * (100 + TOLERANCE_PERCENT) / 100))
# The tone the dialplan plays sits an order of magnitude above this; silence
# and codec dither sit far below it.
MEAN_ABS_MIN=1000

cli() { "$FS_CLI" -H 127.0.0.1 -x "$1"; }

fail() {
  echo "SMOKE FAIL: $1" >&2
  exit 1
}

field() {
  printf '%s' "$2" | sed -n "s/.*\"$1\":\\([0-9][0-9]*\\).*/\\1/p"
}

expect_num() {
  value=$(field "$1" "$3")
  [ -n "$value" ] || fail "$2: $1 missing from $3"
  echo "$value"
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
  # echo, not park: park never writes a frame, so the WRITE_REPLACE bug the
  # playback path lives on would never fire
  cli "bgapi originate {origination_uuid=$uuid}loopback/rig-tone &echo" >/dev/null
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

echo "streaming for ${STREAM_SECONDS}s"
sleep "$STREAM_SECONDS"

status=$(cli "audio_fork status")
echo "status: $status"
forks=$(expect_num forks "streaming" "$status")
[ "$forks" -eq "$CALLS" ] || fail "expected $CALLS forks while streaming, got $forks"
sent=$(expect_num sent_bytes "streaming" "$status")
[ "$sent" -gt 0 ] || fail "no audio was sent: sent_bytes=$sent"
played=$(expect_num playback_bytes_played "streaming" "$status")
[ "$played" -gt 0 ] || fail "server audio never reached the caller: playback_bytes_played=$played"

for uuid in $uuids; do
  cli "uuid_audio_fork $uuid stop" >/dev/null || true
  cli "uuid_kill $uuid" >/dev/null || true
done
sleep 3

final=$(cli "audio_fork status")
echo "final: $final"
final_forks=$(expect_num forks "teardown" "$final")
[ "$final_forks" -eq 0 ] || fail "forks still active after teardown: $final"
leased=$(expect_num pool_leased_slabs "teardown" "$final")
[ "$leased" -eq 0 ] || fail "slabs still leased after teardown: $final"

[ -f "$REPORT" ] || fail "mock server wrote no report at $REPORT"
report=$(cat "$REPORT")
echo "mock report: $report"

case "$report" in
  *'"protocol_errors":[]'*) ;;
  *) fail "server recorded protocol errors: $report" ;;
esac

connections=$(expect_num connections "report" "$report")
[ "$connections" -eq "$CALLS" ] || fail "expected $CALLS connections, got $connections"
hellos=$(expect_num hello_count "report" "$report")
[ "$hellos" -eq "$CALLS" ] || fail "expected $CALLS hellos, got $hellos"
case "$report" in
  *'"rate":16000'*) ;;
  *) fail "hello did not carry rate 16000: $report" ;;
esac
case "$report" in
  *'"channels":1'*) ;;
  *) fail "hello did not carry channels 1: $report" ;;
esac
case "$report" in
  *'"encoding":"L16"'*) ;;
  *) fail "hello did not carry encoding L16: $report" ;;
esac
case "$report" in
  *'"metadata":{"rig":true}'*) ;;
  *) fail "hello did not carry the metadata the rig passed: $report" ;;
esac

# Every fork is started and stopped in the same order, so each connection gets
# the same stream window: at least STREAM_SECONDS, plus however long the two
# fs_cli loops took. That upper end is unbounded on a loaded runner, so the
# ±30% envelope is asserted on bytes per second and the window only has to
# cover the STREAM_SECONDS the script asked for.
span=$(expect_num audio_span_ms_min "report" "$report")
[ "$span" -ge $((STREAM_SECONDS * 1000 - 500)) ] ||
  fail "shortest stream spanned only ${span}ms, expected at least ${STREAM_SECONDS}s"
bytes_min=$(expect_num audio_bytes_min "report" "$report")
floor=$((STREAM_SECONDS * WIRE_BYTES_PER_SECOND * (100 - TOLERANCE_PERCENT) / 100))
[ "$bytes_min" -ge "$floor" ] ||
  fail "quietest connection received $bytes_min bytes, expected at least $floor"
rate_min=$(expect_num audio_rate_min "report" "$report")
[ "$rate_min" -ge "$RATE_MIN" ] ||
  fail "slowest connection ran at $rate_min B/s, expected at least $RATE_MIN"
rate_max=$(expect_num audio_rate_max "report" "$report")
[ "$rate_max" -le "$RATE_MAX" ] ||
  fail "fastest connection ran at $rate_max B/s, expected at most $RATE_MAX"
mean_abs=$(expect_num audio_mean_abs_min "report" "$report")
[ "$mean_abs" -ge "$MEAN_ABS_MIN" ] ||
  fail "quietest connection carried near-silence: mean |sample| $mean_abs"

playback_sent=$(expect_num playback_bytes_min "report" "$report")
[ "$playback_sent" -gt 0 ] ||
  fail "server sent no playback audio on some connection: $playback_sent"

echo "SMOKE PASS"
