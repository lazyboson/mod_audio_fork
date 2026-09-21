#!/bin/sh
# Release gate (DESIGN.md §10, decision 18): continuous churn at CONCURRENT for
# SOAK_HOURS, passing only if RSS and fd stay inside the bands of the
# post-warm-up idle baseline for the whole run, not just at the end.
set -eu

. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/rig_common.sh"

CONCURRENT=${CONCURRENT:-800}
SOAK_HOURS=${SOAK_HOURS:-48}
CALL_RATE=${CALL_RATE:-50}
HOLD_SECONDS=${HOLD_SECONDS:-60}
# One full wave at the target concurrency, for the reason run_load.sh gives.
WARMUP_CALLS=${WARMUP_CALLS:-$CONCURRENT}
MAX_FAILED_CALLS=${MAX_FAILED_CALLS:-0}
SAMPLE_SECONDS=${SAMPLE_SECONDS:-60}
RSS_TOLERANCE_PERCENT=${RSS_TOLERANCE_PERCENT:-5}
FD_TOLERANCE=${FD_TOLERANCE:-10}
SETTLE_SECONDS=${SETTLE_SECONDS:-60}
IDLE_TIMEOUT_SECONDS=${IDLE_TIMEOUT_SECONDS:-600}

CSV=${CSV:-$LOG_DIR/soak-samples.csv}
SUMMARY=$LOG_DIR/soak-summary.txt
SIPP_STAT=$LOG_DIR/sipp-soak.csv
PLAY_LOOPS=$(( (HOLD_SECONDS + 6) / 7 ))
SOAK_SECONDS=$(python3 -c 'import sys; print(int(float(sys.argv[1]) * 3600))' "$SOAK_HOURS")

mkdir -p "$LOG_DIR"
rm -f "$CSV" "$SUMMARY" "$LOG_DIR"/sipp-soak*.csv "$LOG_DIR"/sipp-soak*.log
sed "s/@PLAY_LOOPS@/$PLAY_LOOPS/" "$RIG_DIR/load/uac_tone.xml" > "$LOG_DIR/uac_tone.xml"
grep -q '@PLAY_LOOPS@' "$LOG_DIR/uac_tone.xml" &&
  fail "the scenario's play-loop placeholder was not substituted"

trap 'collect_logs soak-rig-logs.txt; compose down -v >/dev/null 2>&1 || true' EXIT

compose build freeswitch
compose build sipp
compose up -d freeswitch
wait_for_module

sipp_run() {
  compose run --rm --entrypoint sh sipp -c \
    "sipp -i \$(hostname -i) -sf /shared/logs/uac_tone.xml -s rig-sipp freeswitch:5060 \
       -l $CONCURRENT -m $1 -r $CALL_RATE -rp 1000 -mp 6000 \
       -trace_stat -stf /shared/logs/$2.csv -fd 60 -trace_err -error_file /shared/logs/$2-err.log \
       -nostdin" > "$LOG_DIR/$2.log" 2>&1
}

start_sampler "$(basename "$CSV")" "$SAMPLE_SECONDS"

echo "warm-up: $WARMUP_CALLS calls at $CONCURRENT concurrent"
sipp_run "$WARMUP_CALLS" sipp-soak-warmup || fail "the warm-up SIPp run exited $?"
wait_for_idle
sleep "$SETTLE_SECONDS"
baseline_rss=$(fs_rss_kb)
baseline_fd=$(fs_fd_count)
hellos_before=$(jnum hello_count "$(compose exec -T freeswitch cat /shared/mock-report.json)")
# The sampler starts before the warm-up, so the rows it has already written
# predate the baseline and cannot be judged against it.
baseline_rows=$(compose exec -T freeswitch wc -l "/shared/$(basename "$CSV")" |
  awk '{ print $1 }' | tr -d '\r')
echo "baseline at idle: rss=${baseline_rss}kB fds=$baseline_fd hellos=$hellos_before"

# One call is HOLD_SECONDS long, so CONCURRENT/HOLD_SECONDS calls a second keeps
# the box full; -m is what turns that rate into a run of the requested length.
soak_calls=$(( SOAK_SECONDS * CONCURRENT / HOLD_SECONDS ))
echo "soaking for ${SOAK_HOURS}h: $soak_calls calls at $CONCURRENT concurrent"
sipp_run "$soak_calls" sipp-soak && sipp_code=0 || sipp_code=$?
echo "sipp exit code: $sipp_code"

wait_for_idle
sleep "$SETTLE_SECONDS"
fetch_samples "$(basename "$CSV")"

final=$(status_json)
report=$(compose exec -T freeswitch cat /shared/mock-report.json)
final_rss=$(fs_rss_kb)
final_fd=$(fs_fd_count)

failed_calls=$(python3 "$RIG_DIR/load/sipp_failed.py" "$SIPP_STAT")
forks=$(jnum forks "$final")
leased=$(jnum pool_leased_slabs "$final")
hellos=$(( $(jnum hello_count "$report") - hellos_before ))

rss_low=$((baseline_rss * (100 - RSS_TOLERANCE_PERCENT) / 100))
rss_high=$((baseline_rss * (100 + RSS_TOLERANCE_PERCENT) / 100))
fd_low=$((baseline_fd - FD_TOLERANCE))
fd_high=$((baseline_fd + FD_TOLERANCE))

# The gate is the whole curve, not the last point: a leak that plateaus inside
# the band at the end still has to have stayed inside it throughout. Only the
# idle rows count, since RSS under traffic is concurrency, not growth.
breaches=$(awk -F, -v lo="$rss_low" -v hi="$rss_high" -v flo="$fd_low" -v fhi="$fd_high" \
  -v skip="$baseline_rows" '
  NR > skip && $2 == 0 && $9 != "" {
    idle++
    if ($9 < lo || $9 > hi || $10 < flo || $10 > fhi) { print "  " $0; breach++ }
  }
  END { print "idle_rows=" idle+0 " breaches=" breach+0 }' "$CSV")
idle_rows=$(printf '%s\n' "$breaches" | sed -n 's/.*idle_rows=\([0-9]*\).*/\1/p')
breach_count=$(printf '%s\n' "$breaches" | sed -n 's/.*breaches=\([0-9]*\)/\1/p')

{
  echo "soak: ${SOAK_HOURS}h at $CONCURRENT concurrent, $soak_calls calls, ${PLAY_LOOPS}x7s hold."
  echo "SIPp exit $sipp_code, failed calls $failed_calls (max $MAX_FAILED_CALLS)."
  echo "Mock saw $hellos hellos over the soak."
  echo "After teardown: forks=$forks leased_slabs=$leased."
  echo "RSS ${final_rss}kB against a ${baseline_rss}kB baseline (band ${rss_low}-${rss_high});"
  echo "fds $final_fd against $baseline_fd (band ${fd_low}-${fd_high})."
  echo "Idle samples inside the bands: $((idle_rows - breach_count)) of $idle_rows."
  printf '%s\n' "$breaches" | grep -v '^idle_rows=' || true
} | tee "$SUMMARY"

[ "$failed_calls" -le "$MAX_FAILED_CALLS" ] ||
  fail "SIPp reported $failed_calls failed calls, allowed $MAX_FAILED_CALLS"
[ "$hellos" -eq "$soak_calls" ] ||
  fail "mock counted $hellos hellos over the soak, expected $soak_calls"
case "$report" in
  *'"protocol_errors":[]'*) ;;
  *) fail "mock recorded protocol errors" ;;
esac
[ "$forks" -eq 0 ] || fail "forks still active after the soak: $forks"
[ "$leased" -eq 0 ] || fail "slabs still leased after the soak: $leased"
[ "${idle_rows:-0}" -ge 2 ] ||
  fail "only $idle_rows idle samples in the soak; the curve cannot be judged"
[ "${breach_count:-1}" -eq 0 ] ||
  fail "$breach_count idle samples left the RSS/fd bands during the soak"
[ "$final_rss" -ge "$rss_low" ] && [ "$final_rss" -le "$rss_high" ] ||
  fail "RSS ${final_rss}kB is outside ${RSS_TOLERANCE_PERCENT}% of the ${baseline_rss}kB baseline"
[ "$final_fd" -ge "$fd_low" ] && [ "$final_fd" -le "$fd_high" ] ||
  fail "fd count $final_fd is more than $FD_TOLERANCE from the $baseline_fd baseline"

shutdown_and_assert_exit
echo "samples: $CSV"
echo "SOAK PASS"
