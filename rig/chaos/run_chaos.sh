#!/bin/sh
# Chaos matrix (DESIGN.md §10 decision 18, behaviours in §7). Each scenario arms
# its fault, drives forks, asserts the observable §7 names with the value it saw,
# clears the fault and leaves the module at zero forks before the next one runs.
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/../load/rig_common.sh"

ALL_SCENARIOS="vendor_stall half_open_tcp reconnect_storm global_cap_exhaustion
barge_in_flood json_fuzz_replay tls_failures hangup_every_state"
SCENARIOS=${SCENARIOS:-$ALL_SCENARIOS}
CALLS=${CALLS:-50}
STORM_FORKS=${STORM_FORKS:-1000}
CAP_FORKS=${CAP_FORKS:-20}
GLOBAL_CAP_MB=${GLOBAL_CAP_MB:-1}
BARGE_HZ=${BARGE_HZ:-50}
BARGE_SECONDS=${BARGE_SECONDS:-10}
FUZZ_MUTATIONS=${FUZZ_MUTATIONS:-200}
IDLE_TIMEOUT_SECONDS=${IDLE_TIMEOUT_SECONDS:-300}

TABLE=$LOG_DIR/chaos-table.txt
PROXY_URL=ws://toxiproxy:9099/
DIRECT_URL=ws://mock-ws:9099/
TLS_URL=wss://mock-wss:9443/

mkdir -p "$LOG_DIR"
: > "$TABLE"

results=""
record() {
  printf '%-24s %-6s %s\n' "$1" "$2" "$3" >> "$TABLE"
  results="$results$1=$2 "
}

say() { echo "  $*"; }

fs_batch() { compose exec -T freeswitch sh /srv/load/fs_batch.sh; }

# The host mints the uuids so a batch needs no round trip per call: fs_cli's
# create_uuid would cost one exec each.
new_uuids() {
  python3 -c 'import sys, uuid
for _ in range(int(sys.argv[1])):
    print(uuid.uuid4())' "$1"
}

originate() {
  for u in $1; do
    echo "bgapi originate {origination_uuid=$u}loopback/rig-tone &echo"
  done | fs_batch > /dev/null
}

start_forks() {
  for u in $1; do
    echo "uuid_audio_fork $u start $2 mono 16000 {\"chaos\":true}"
  done | fs_batch
}

kill_calls() {
  for u in $1; do
    echo "uuid_audio_fork $u stop"
    echo "uuid_kill $u"
  done | fs_batch > /dev/null
}

mock_report() { compose exec -T freeswitch cat "${1:-/shared/mock-report.json}" 2>/dev/null || echo '{}'; }

# Recreating the mock both arms the fault and hands the scenario an empty
# report, so no scenario has to subtract the one before it.
arm_mock() {
  compose exec -T freeswitch rm -f /shared/mock-report.json /shared/mock-wss-report.json
  MOCK_WS_PLAYBACK_MS=2000 MOCK_WS_STALL_AFTER=0 MOCK_WS_DROP_AFTER=0
  MOCK_WS_CLEAR_MARK_HZ=0 MOCK_WS_CLEAR_MARK_SECONDS=0
  MOCK_WS_FUZZ_DIR= MOCK_WS_FUZZ_MUTATIONS=0
  export MOCK_WS_PLAYBACK_MS MOCK_WS_STALL_AFTER MOCK_WS_DROP_AFTER \
    MOCK_WS_CLEAR_MARK_HZ MOCK_WS_CLEAR_MARK_SECONDS MOCK_WS_FUZZ_DIR MOCK_WS_FUZZ_MUTATIONS
  for assignment in "$@"; do export "$assignment"; done
  compose up -d --force-recreate mock-ws >/dev/null 2>&1
  sleep 3
}

# The cap and the CA are read at module load, so changing either means a new
# FreeSWITCH; every scenario that does so puts the shipped values back.
recreate_freeswitch() {
  AUDIOFORK_GLOBAL_CAP_MB=${1:-}
  AUDIOFORK_TLS_CA=${2:-/srv/tls/ca.pem}
  export AUDIOFORK_GLOBAL_CAP_MB AUDIOFORK_TLS_CA
  compose up -d --force-recreate freeswitch >/dev/null 2>&1
  wait_for_module
}

toxi_reset() { toxi /reset >/dev/null; }

add_toxic() {
  toxi /proxies/ws/toxics \
    "{\"name\":\"$1\",\"type\":\"$1\",\"stream\":\"$2\",\"attributes\":$3}" >/dev/null
}

expect_idle() {
  wait_for_idle
  status=$(status_json)
  forks=$(jnum forks "$status")
  leased=$(jnum pool_leased_slabs "$status")
  say "after teardown: forks=$forks leased_slabs=$leased"
  [ "$forks" = "0" ] || fail "$1: forks still active: $forks"
  [ "$leased" = "0" ] || fail "$1: slabs still leased: $leased"
}

scenario_vendor_stall() {
  arm_mock MOCK_WS_STALL_AFTER=5 MOCK_WS_PLAYBACK_MS=0
  uuids=$(new_uuids "$CALLS")
  originate "$uuids"
  sleep 3
  start_forks "$uuids" "$DIRECT_URL" > /dev/null
  # send-buffer-seconds is 10, so the ring cannot start dropping before then
  sleep 25
  status=$(status_json)
  dropped=$(jnum buffer_dropped_bytes "$status")
  say "buffer_dropped_bytes=$dropped forks=$(jnum forks "$status")"
  kill_calls "$uuids"
  [ "${dropped:-0}" -gt 0 ] ||
    fail "vendor_stall: a stalled peer dropped nothing: buffer_dropped_bytes=$dropped"
  expect_idle vendor_stall
  record vendor_stall PASS "buffer_dropped_bytes=$dropped over $CALLS stalled forks"
}

scenario_half_open_tcp() {
  arm_mock MOCK_WS_PLAYBACK_MS=0
  toxi_reset
  uuids=$(new_uuids "$CALLS")
  originate "$uuids"
  sleep 3
  start_forks "$uuids" "$PROXY_URL" > /dev/null
  sleep 5
  # timeout 0 holds the data without closing; toxiproxy closes the held
  # connections when the toxic goes away, which is the half-open recovery
  add_toxic timeout downstream '{"timeout":0}'
  sleep 12
  toxi_reset
  sleep 15
  status=$(status_json)
  reconnects=$(jnum reconnects "$status")
  say "reconnects=$reconnects forks=$(jnum forks "$status")"
  kill_calls "$uuids"
  sleep 5
  resumes=$(jnum resume_count "$(mock_report)")
  say "mock resume_count=$resumes"
  [ "${reconnects:-0}" -ge 1 ] || fail "half_open_tcp: no reconnect: $reconnects"
  [ "${resumes:-0}" -ge 1 ] || fail "half_open_tcp: mock saw no resume: $resumes"
  expect_idle half_open_tcp
  record half_open_tcp PASS "reconnects=$reconnects resume_count=$resumes"
}

scenario_reconnect_storm() {
  arm_mock MOCK_WS_PLAYBACK_MS=0
  toxi_reset
  uuids=$(new_uuids "$STORM_FORKS")
  originate "$uuids"
  sleep 10
  start_forks "$uuids" "$PROXY_URL" > /dev/null
  sleep 15
  add_toxic reset_peer downstream '{"timeout":0}'
  sleep 3
  # cleared straight away: left on, every backoff retry would be RST too and
  # the hello count would never settle at two per fork
  toxi_reset
  sleep 40
  status=$(status_json)
  reconnects=$(jnum reconnects "$status")
  say "reconnects=$reconnects of $STORM_FORKS forks"
  kill_calls "$uuids"
  sleep 8
  report=$(mock_report)
  hellos=$(jnum hello_count "$report")
  spread=$(jnum reconnect_spread_ms "$report")
  say "mock hello_count=$hellos (want $((STORM_FORKS * 2))) reconnect_spread_ms=$spread"
  [ "${reconnects:-0}" -eq "$STORM_FORKS" ] ||
    fail "reconnect_storm: $reconnects of $STORM_FORKS forks reconnected"
  [ "${hellos:-0}" -eq "$((STORM_FORKS * 2))" ] ||
    fail "reconnect_storm: mock counted $hellos hellos, expected $((STORM_FORKS * 2))"
  [ "${spread:-0}" -ge 100 ] ||
    fail "reconnect_storm: first reconnects spread only ${spread}ms, expected >= 100"
  expect_idle reconnect_storm
  record reconnect_storm PASS "hellos=$hellos reconnects=$reconnects spread=${spread}ms"
}

scenario_global_cap_exhaustion() {
  arm_mock MOCK_WS_STALL_AFTER=5 MOCK_WS_PLAYBACK_MS=0
  recreate_freeswitch "$GLOBAL_CAP_MB"
  uuids=$(new_uuids "$CAP_FORKS")
  originate "$uuids"
  sleep 3
  replies=$(start_forks "$uuids" "$DIRECT_URL")
  sleep 20
  status=$(status_json)
  refused=$(printf '%s\n' "$replies" | grep -c 'global memory cap reached' || true)
  start_failed=$(jnum start_failed "$status")
  degraded=$(jnum degraded_forks "$status")
  say "cap ${GLOBAL_CAP_MB}MB over $CAP_FORKS forks: -ERR replies=$refused"
  say "start_failed=$start_failed degraded_forks=$degraded"
  kill_calls "$uuids"
  [ "${refused:-0}" -ge 1 ] ||
    fail "global_cap_exhaustion: no start was refused with -ERR global memory cap reached"
  [ "${start_failed:-0}" -ge 1 ] || [ "${degraded:-0}" -ge 1 ] ||
    fail "global_cap_exhaustion: start_failed=$start_failed degraded_forks=$degraded, wanted one >= 1"
  expect_idle global_cap_exhaustion
  recreate_freeswitch
  record global_cap_exhaustion PASS \
    "refused=$refused start_failed=$start_failed degraded_forks=$degraded"
}

scenario_barge_in_flood() {
  arm_mock MOCK_WS_PLAYBACK_MS=20000 \
    "MOCK_WS_CLEAR_MARK_HZ=$BARGE_HZ" "MOCK_WS_CLEAR_MARK_SECONDS=$BARGE_SECONDS"
  uuids=$(new_uuids "$CALLS")
  originate "$uuids"
  sleep 3
  start_forks "$uuids" "$DIRECT_URL" > /dev/null
  # read after the flood has finished, so the mock cannot send another clear
  # between the status read and the report the count is compared against
  sleep $((BARGE_SECONDS + 8))
  status=$(status_json)
  barge_ins=$(jnum barge_ins "$status")
  texts_dropped=$(jnum pending_texts_dropped "$status")
  say "barge_ins=$barge_ins pending_texts_dropped=$texts_dropped"
  kill_calls "$uuids"
  sleep 5
  report=$(mock_report)
  clears=$(jnum clears_sent "$report")
  say "mock clears_sent=$clears marks_sent=$(jnum marks_sent "$report")"
  [ "${barge_ins:-0}" -eq "${clears:-0}" ] ||
    fail "barge_in_flood: module counted $barge_ins barge-ins for $clears clears"
  case "$report" in
    *'"protocol_errors":[]'*) ;;
    *) fail "barge_in_flood: mock recorded protocol errors" ;;
  esac
  expect_idle barge_in_flood
  record barge_in_flood PASS "barge_ins=$barge_ins == clears_sent=$clears"
}

scenario_json_fuzz_replay() {
  arm_mock MOCK_WS_PLAYBACK_MS=0 MOCK_WS_FUZZ_DIR=/srv/corpus \
    "MOCK_WS_FUZZ_MUTATIONS=$FUZZ_MUTATIONS"
  uuids=$(new_uuids "$CALLS")
  originate "$uuids"
  sleep 3
  start_forks "$uuids" "$DIRECT_URL" > /dev/null
  sleep 20
  status=$(status_json)
  [ -n "$status" ] || fail "json_fuzz_replay: FreeSWITCH stopped answering"
  say "alive under the corpus plus $FUZZ_MUTATIONS mutations: forks=$(jnum forks "$status")"
  kill_calls "$uuids"
  expect_idle json_fuzz_replay
  # audio_fork status carries no json_error counter, so surviving the replay and
  # returning to zero forks is the whole observable here; the ::json_error event
  # is the only per-message signal and the rig does not subscribe to events.
  record json_fuzz_replay PASS "survived; no json_error counter in status to assert on"
}

scenario_tls_failures() {
  arm_mock MOCK_WS_PLAYBACK_MS=0
  recreate_freeswitch "" /srv/tls/other-ca.pem
  uuids=$(new_uuids "$CALLS")
  originate "$uuids"
  sleep 3
  start_forks "$uuids" "$TLS_URL" > /dev/null
  sleep 30
  failures=$(jnum tls_handshake_failures "$(mock_report /shared/mock-wss-report.json)")
  say "mock-wss tls_handshake_failures=$failures after 30s of retries"
  [ "${failures:-0}" -ge 2 ] ||
    fail "tls_failures: only $failures handshake failures, expected the fork to keep retrying"
  kill_calls "$uuids"
  expect_idle tls_failures
  recreate_freeswitch
  record tls_failures PASS "tls_handshake_failures=$failures, stop cleaned up to 0 forks"
}

# Hangup in each of the four states §7's last row funnels to DRAINING: before
# the socket is up, while streaming, while reconnecting, and mid-drain.
scenario_hangup_every_state() {
  arm_mock MOCK_WS_PLAYBACK_MS=0
  toxi_reset

  connecting=$(new_uuids "$CALLS")
  originate "$connecting"
  sleep 3
  add_toxic latency downstream '{"latency":3000,"jitter":0}'
  start_forks "$connecting" "$PROXY_URL" > /dev/null
  sleep 1
  kill_calls "$connecting"
  toxi_reset
  say "hung up $CALLS forks 1s into a 3s handshake latency"
  expect_idle "hangup_every_state/connecting"

  active=$(new_uuids "$CALLS")
  originate "$active"
  sleep 3
  start_forks "$active" "$DIRECT_URL" > /dev/null
  sleep 8
  kill_calls "$active"
  say "hung up $CALLS forks mid-stream"
  expect_idle "hangup_every_state/active"

  reconnecting=$(new_uuids "$CALLS")
  originate "$reconnecting"
  sleep 3
  start_forks "$reconnecting" "$PROXY_URL" > /dev/null
  sleep 6
  add_toxic timeout downstream '{"timeout":0}'
  sleep 4
  kill_calls "$reconnecting"
  toxi_reset
  say "hung up $CALLS forks while they were reconnecting"
  expect_idle "hangup_every_state/reconnecting"

  draining=$(new_uuids "$CALLS")
  originate "$draining"
  sleep 3
  start_forks "$draining" "$DIRECT_URL" > /dev/null
  sleep 6
  # stop and kill in one batch, so the kill lands inside the drain the stop began
  for u in $draining; do
    echo "uuid_audio_fork $u stop"
    echo "uuid_kill $u"
  done | fs_batch > /dev/null
  say "killed $CALLS forks inside the drain their stop began"
  expect_idle "hangup_every_state/draining"
  record hangup_every_state PASS "connecting, active, reconnecting and draining all reached 0 forks"
}

trap 'collect_logs chaos-rig-logs.txt; compose down -v >/dev/null 2>&1 || true' EXIT

compose build freeswitch
compose up -d freeswitch
wait_for_module
toxi /proxies '{"name":"ws","listen":"0.0.0.0:9099","upstream":"mock-ws:9099","enabled":true}' >/dev/null

for name in $SCENARIOS; do
  case " $ALL_SCENARIOS " in
    *" $name "*) ;;
    *) fail "unknown scenario: $name" ;;
  esac
  echo "== $name"
  "scenario_$name"
done

echo
echo "scenario                 result details"
cat "$TABLE"
shutdown_and_assert_exit
echo "CHAOS PASS: $results"
