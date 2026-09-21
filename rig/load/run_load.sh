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
WARMUP_CALLS=${WARMUP_CALLS:-200}
IDLE_TIMEOUT_SECONDS=${IDLE_TIMEOUT_SECONDS:-300}
SAMPLE_SECONDS=${SAMPLE_SECONDS:-5}
RSS_TOLERANCE_PERCENT=${RSS_TOLERANCE_PERCENT:-5}
FD_TOLERANCE=${FD_TOLERANCE:-10}
SETTLE_SECONDS=${SETTLE_SECONDS:-20}

CSV=${CSV:-$LOG_DIR/load-samples.csv}
SUMMARY=$LOG_DIR/load-summary.txt
SIPP_STAT=$LOG_DIR/sipp-load.csv
# One pcap play is 7 s; the hold time is rounded up to that granularity.
PLAY_LOOPS=$(( (HOLD_SECONDS + 6) / 7 ))

mkdir -p "$LOG_DIR"
rm -f "$CSV" "$SUMMARY" "$LOG_DIR"/sipp-*.csv "$LOG_DIR"/sipp-*.log
sed "s/@PLAY_LOOPS@/$PLAY_LOOPS/" "$RIG_DIR/load/uac_tone.xml" > "$LOG_DIR/uac_tone.xml"
if grep -q '@PLAY_LOOPS@' "$LOG_DIR/uac_tone.xml"; then
  fail "the scenario's play-loop placeholder was not substituted"
fi

trap 'collect_logs load-rig-logs.txt; compose down -v >/dev/null 2>&1 || true' EXIT

compose build freeswitch
compose build sipp
compose up -d freeswitch
wait_for_module

sipp_run() {
  compose run --rm --entrypoint sh sipp -c \
    "sipp -i \$(hostname -i) -sf /shared/logs/uac_tone.xml -s rig-sipp freeswitch:5060 \
       -l $CONCURRENT -m $1 -r $CALL_RATE -rp 1000 -mp 6000 \
       -trace_stat -stf /shared/logs/$2.csv -fd 5 -trace_err -error_file /shared/logs/$2-err.log \
       -nostdin" > "$LOG_DIR/$2.log" 2>&1
}

mock_report() {
  compose exec -T freeswitch cat /shared/mock-report.json
}

start_sampler "$(basename "$CSV")" "$SAMPLE_SECONDS"

echo "warm-up: $WARMUP_CALLS calls"
sipp_run "$WARMUP_CALLS" sipp-warmup || fail "the warm-up SIPp run exited $?"
wait_for_idle
sleep "$SETTLE_SECONDS"
# Baseline at idle after the warm-up, against which the equally idle final
# sample is compared: FreeSWITCH's first calls grow session pools and thread
# stacks that never shrink, and a baseline taken under traffic measures
# concurrency rather than a leak.
baseline_rss=$(fs_rss_kb)
baseline_fd=$(fs_fd_count)
hellos_before=$(jnum hello_count "$(mock_report)")
echo "baseline at idle: rss=${baseline_rss}kB fds=$baseline_fd hellos=$hellos_before"

echo "starting SIPp: $TOTAL_CALLS calls, $CONCURRENT concurrent, ${CALL_RATE}/s, ${PLAY_LOOPS}x7s hold"
sipp_run "$TOTAL_CALLS" sipp-load && sipp_code=0 || sipp_code=$?
echo "sipp exit code: $sipp_code"

wait_for_idle
echo "settling for ${SETTLE_SECONDS}s before the leak assertions"
sleep "$SETTLE_SECONDS"
fetch_samples "$(basename "$CSV")"

final=$(status_json)
report=$(mock_report)
final_rss=$(fs_rss_kb)
final_fd=$(fs_fd_count)
echo "final status: $final"

# audio_fork status sums its byte counters over the live registry, so every one
# of them reads 0 once the last fork retires. What the run drove is the peak of
# the samples taken while it was driving.
peak() {
  awk -F, -v col="$1" 'NR > 1 && $col > m { m = $col } END { print m + 0 }' "$CSV"
}

failed_calls=$(python3 "$RIG_DIR/load/sipp_failed.py" "$SIPP_STAT")
forks=$(jnum forks "$final")
leased=$(jnum pool_leased_slabs "$final")
hellos=$(( $(jnum hello_count "$report") - hellos_before ))
audio_bytes=$(jnum audio_bytes "$report")
peak_forks=$(peak 2)
peak_sent=$(peak 3)
peak_media_dropped=$(peak 4)
peak_buffer_dropped=$(peak 5)
peak_reconnects=$(peak 6)

rss_low=$((baseline_rss * (100 - RSS_TOLERANCE_PERCENT) / 100))
rss_high=$((baseline_rss * (100 + RSS_TOLERANCE_PERCENT) / 100))
fd_low=$((baseline_fd - FD_TOLERANCE))
fd_high=$((baseline_fd + FD_TOLERANCE))

{
  echo "load tier: $TOTAL_CALLS calls at $CONCURRENT concurrent, ${CALL_RATE}/s, ${PLAY_LOOPS}x7s hold."
  echo "SIPp exit $sipp_code, failed calls $failed_calls (max $MAX_FAILED_CALLS)."
  echo "Mock saw $hellos hellos and $audio_bytes cumulative audio bytes."
  echo "Peaks while driving: forks=$peak_forks sent_bytes=$peak_sent reconnects=$peak_reconnects"
  echo "buffer_dropped=$peak_buffer_dropped media_dropped=$peak_media_dropped."
  echo "After teardown: forks=$forks leased_slabs=$leased."
  echo "RSS ${final_rss}kB against a ${baseline_rss}kB baseline (band ${rss_low}-${rss_high});"
  echo "fds $final_fd against $baseline_fd (band ${fd_low}-${fd_high})."
} | tee "$SUMMARY"

[ "$failed_calls" -le "$MAX_FAILED_CALLS" ] ||
  fail "SIPp reported $failed_calls failed calls, allowed $MAX_FAILED_CALLS"
[ "$sipp_code" -eq 0 ] || [ "$MAX_FAILED_CALLS" -gt 0 ] ||
  fail "SIPp exited $sipp_code"
[ "$hellos" -eq "$TOTAL_CALLS" ] ||
  fail "mock counted $hellos hellos over the run, expected $TOTAL_CALLS"
[ "$peak_sent" -gt 0 ] || fail "the module never sent a byte: peak sent_bytes 0"
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
