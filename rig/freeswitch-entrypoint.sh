#!/bin/sh
# The repo's conf/audio_fork.conf.xml stays the rig's only config source, but
# the wss leg needs a CA path that only exists inside the container, and
# FreeSWITCH does not expand $${vars} we could set from the rig tree into a
# param value. So the shipped file is mounted as a template and the one value
# is substituted in before FreeSWITCH reads it.
set -eu

TEMPLATE=/srv/audio_fork.conf.xml.in
CONFIG=/srv/audio_fork.conf.xml
CA=/srv/tls/ca.pem

[ -f "$TEMPLATE" ] || { echo "no config template at $TEMPLATE" >&2; exit 1; }
[ -f "$CA" ] || { echo "no test CA at $CA" >&2; exit 1; }

sed 's#name="tls-ca-file" value=""#name="tls-ca-file" value="'"$CA"'"#' \
  "$TEMPLATE" > "$CONFIG"

grep -q "value=\"$CA\"" "$CONFIG" ||
  { echo "tls-ca-file was not substituted; did the shipped param change?" >&2; exit 1; }

exec freeswitch -nf -nonat -nosql
