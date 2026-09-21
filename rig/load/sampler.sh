#!/bin/sh
# Runs inside the FreeSWITCH container for the life of a run. Sampling from the
# host costs one `docker compose exec` per value, which takes tens of seconds
# once the box is loaded: rows then carry a timestamp minutes newer than the
# counters in them, and the drain looks like a leak. One in-place process reads
# the status, RSS and fd count back to back and dates the row where it is read.
set -eu

OUT=$1
INTERVAL=$2

num() { printf '%s' "$2" | sed -n "s/.*\"$1\":\([0-9][0-9]*\).*/\1/p"; }

echo "time,forks,sent_bytes,media_dropped_bytes,buffer_dropped_bytes,reconnects,pool_leased_slabs,degraded_forks,rss_kb,fds" > "$OUT"

while :; do
  status=$(fs_cli -H 127.0.0.1 -x 'audio_fork status' 2>/dev/null) || status=""
  pid=$(pgrep -x freeswitch | head -1)
  if [ -n "$status" ] && [ -n "$pid" ]; then
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$(date -u +%H:%M:%S)" \
      "$(num forks "$status")" "$(num sent_bytes "$status")" \
      "$(num media_dropped_bytes "$status")" "$(num buffer_dropped_bytes "$status")" \
      "$(num reconnects "$status")" "$(num pool_leased_slabs "$status")" \
      "$(num degraded_forks "$status")" \
      "$(awk '/VmRSS/ { print $2 }' "/proc/$pid/status")" \
      "$(ls "/proc/$pid/fd" | wc -l)" >> "$OUT"
  fi
  sleep "$INTERVAL"
done
