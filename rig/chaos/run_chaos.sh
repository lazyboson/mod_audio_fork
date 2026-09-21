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
  env "$@" compose up -d --force-recreate mock-ws >/dev/null 2>&1
  sleep 3
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
