#!/bin/sh
# Nightly load tier (DESIGN.md §10, decision 17): SIPp drives TOTAL_CALLS through
# FreeSWITCH at CONCURRENT concurrency, every call forked from the dialplan, and
# the run ends on the hard leak assertions — counters back to zero, RSS and fd
# back to the post-warm-up baseline.
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/rig_common.sh"

CONCURRENT=${CONCURRENT:-1000}
TOTAL_CALLS=${TOTAL_CALLS:-10000}
CALL_RATE=${CALL_RATE:-50}
HOLD_SECONDS=${HOLD_SECONDS:-60}
MAX_FAILED_CALLS=${MAX_FAILED_CALLS:-0}
BASELINE_AFTER_SECONDS=${BASELINE_AFTER_SECONDS:-60}
SAMPLE_SECONDS=${SAMPLE_SECONDS:-5}
RSS_TOLERANCE_PERCENT=${RSS_TOLERANCE_PERCENT:-5}
FD_TOLERANCE=${FD_TOLERANCE:-10}
SETTLE_SECONDS=${SETTLE_SECONDS:-20}

CSV=${CSV:-$LOG_DIR/load-samples.csv}
SUMMARY=$LOG_DIR/load-summary.txt
SIPP_STAT=$LOG_DIR/sipp-stat.csv
# One pcap play is 7 s; the hold time is rounded up to that granularity.
PLAY_LOOPS=$(( (HOLD_SECONDS + 6) / 7 ))

mkdir -p "$LOG_DIR"
rm -f "$CSV" "$SUMMARY" "$SIPP_STAT" "$LOG_DIR"/sipp-stat_*.csv
sed "s/@PLAY_LOOPS@/$PLAY_LOOPS/" "$RIG_DIR/load/uac_tone.xml" > "$LOG_DIR/uac_tone.xml"
if grep -q '@PLAY_LOOPS@' "$LOG_DIR/uac_tone.xml"; then
  fail "the scenario's play-loop placeholder was not substituted"
fi

trap 'collect_logs load-rig-logs.txt; compose down -v >/dev/null 2>&1 || true' EXIT

compose build freeswitch
compose build sipp
compose up -d freeswitch
wait_for_module

sample() {
  status=$(status_json)
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(date -u +%H:%M:%S)" \
    "$(jnum forks "$status")" "$(jnum sent_bytes "$status")" \
    "$(jnum media_dropped_bytes "$status")" "$(jnum buffer_dropped_bytes "$status")" \
    "$(jnum reconnects "$status")" "$(jnum pool_leased_slabs "$status")" \
    "$(jnum degraded_forks "$status")" "$(fs_rss_kb)" "$(fs_fd_count)" >> "$CSV"
}

echo "time,forks,sent_bytes,media_dropped_bytes,buffer_dropped_bytes,reconnects,pool_leased_slabs,degraded_forks,rss_kb,fds" > "$CSV"

echo "starting SIPp: $TOTAL_CALLS calls, $CONCURRENT concurrent, ${CALL_RATE}/s, ${PLAY_LOOPS}x7s hold"
compose run --rm --entrypoint sh sipp -c \
  "sipp -i \$(hostname -i) -sf /shared/logs/uac_tone.xml -s rig-sipp freeswitch:5060 \
     -l $CONCURRENT -m $TOTAL_CALLS -r $CALL_RATE -rp 1000 -mp 6000 \
     -trace_stat -stf /shared/logs/sipp-stat.csv -fd 5 -trace_err -error_file /shared/logs/sipp-err.log \
     -nostdin" > "$LOG_DIR/sipp.log" 2>&1 &
sipp_pid=$!

baseline_rss=""
baseline_fd=""
elapsed=0
while kill -0 "$sipp_pid" 2>/dev/null; do
  sample
  elapsed=$((elapsed + SAMPLE_SECONDS))
  if [ -z "$baseline_rss" ] && [ "$elapsed" -ge "$BASELINE_AFTER_SECONDS" ]; then
    # Baseline after warm-up, not at start: FreeSWITCH grows its session pools
    # and the module leases its first slabs over the first minute of traffic.
    baseline_rss=$(fs_rss_kb)
    baseline_fd=$(fs_fd_count)
    echo "baseline after ${elapsed}s: rss=${baseline_rss}kB fds=$baseline_fd"
  fi
  sleep "$SAMPLE_SECONDS"
done
wait "$sipp_pid" && sipp_code=0 || sipp_code=$?
echo "sipp exit code: $sipp_code"

echo "settling for ${SETTLE_SECONDS}s before the leak assertions"
sleep "$SETTLE_SECONDS"
sample

final=$(status_json)
report=$(compose exec -T freeswitch cat /shared/mock-report.json)
final_rss=$(fs_rss_kb)
final_fd=$(fs_fd_count)
echo "final status: $final"

failed_calls=$(python3 "$RIG_DIR/load/sipp_failed.py" "$SIPP_STAT")
forks=$(jnum forks "$final")
leased=$(jnum pool_leased_slabs "$final")
hellos=$(jnum hello_count "$report")
sent=$(jnum sent_bytes "$final")
reconnects=$(jnum reconnects "$final")
buffer_dropped=$(jnum buffer_dropped_bytes "$final")
media_dropped=$(jnum media_dropped_bytes "$final")

[ -n "$baseline_rss" ] || fail "the run ended before the warm-up baseline was taken"
rss_low=$((baseline_rss * (100 - RSS_TOLERANCE_PERCENT) / 100))
rss_high=$((baseline_rss * (100 + RSS_TOLERANCE_PERCENT) / 100))
fd_low=$((baseline_fd - FD_TOLERANCE))
fd_high=$((baseline_fd + FD_TOLERANCE))

{
  echo "load tier: $TOTAL_CALLS calls at $CONCURRENT concurrent, ${CALL_RATE}/s, ${PLAY_LOOPS}x7s hold."
  echo "SIPp exit $sipp_code, failed calls $failed_calls (max $MAX_FAILED_CALLS)."
  echo "Mock saw $hellos hellos; module sent $sent bytes, $reconnects reconnects,"
  echo "$buffer_dropped buffer-dropped and $media_dropped media-dropped bytes."
  echo "After teardown: forks=$forks leased_slabs=$leased."
  echo "RSS ${final_rss}kB against a ${baseline_rss}kB baseline (band ${rss_low}-${rss_high});"
  echo "fds $final_fd against $baseline_fd (band ${fd_low}-${fd_high})."
} | tee "$SUMMARY"

[ "$failed_calls" -le "$MAX_FAILED_CALLS" ] ||
  fail "SIPp reported $failed_calls failed calls, allowed $MAX_FAILED_CALLS"
[ "$sipp_code" -eq 0 ] || [ "$MAX_FAILED_CALLS" -gt 0 ] ||
  fail "SIPp exited $sipp_code"
[ "$hellos" -eq "$TOTAL_CALLS" ] ||
  fail "mock counted $hellos hellos, expected $TOTAL_CALLS"
case "$report" in
  *'"protocol_errors":[]'*) ;;
  *) fail "mock recorded protocol errors" ;;
esac
[ "$forks" -eq 0 ] || fail "forks still active after the run: $forks"
[ "$leased" -eq 0 ] || fail "slabs still leased after the run: $leased"
[ "$final_rss" -ge "$rss_low" ] && [ "$final_rss" -le "$rss_high" ] ||
  fail "RSS ${final_rss}kB is outside ${RSS_TOLERANCE_PERCENT}% of the ${baseline_rss}kB baseline"
[ "$final_fd" -ge "$fd_low" ] && [ "$final_fd" -le "$fd_high" ] ||
  fail "fd count $final_fd is more than $FD_TOLERANCE from the $baseline_fd baseline"

shutdown_and_assert_exit
echo "samples: $CSV"
echo "LOAD PASS"
