#!/bin/sh
# Runs inside the FreeSWITCH container: one fs_cli call per line of stdin,
# echoing "<command>\t<reply>". A compose exec costs seconds on Docker Desktop,
# so a scenario's fifty starts have to travel as one exec rather than fifty.
set -eu

while IFS= read -r line; do
  [ -n "$line" ] || continue
  printf '%s\t%s\n' "$line" "$(fs_cli -H 127.0.0.1 -x "$line" 2>&1 | tr '\n' ' ')"
done
