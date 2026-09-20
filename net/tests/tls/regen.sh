#!/bin/sh
# Regenerates the checked-in test credentials. Run from this directory; the
# output is committed so the suite needs no openssl at build time.
set -eu

cd "$(dirname "$0")"
DAYS=3650

ca() {
  openssl req -x509 -newkey rsa:2048 -nodes -days "$DAYS" \
    -keyout "$1.key" -out "$1.pem" -subj "/CN=$2"
}

leaf() {
  name=$1
  subject=$2
  ext=$3
  openssl req -newkey rsa:2048 -nodes -keyout "$name.key" -out "$name.csr" \
    -subj "/CN=$subject"
  openssl x509 -req -in "$name.csr" -CA ca.pem -CAkey ca.key -CAcreateserial \
    -days "$DAYS" -out "$name.pem" -extfile /dev/stdin <<EXT
$ext
EXT
  rm -f "$name.csr"
}

ca ca "mod_audio_fork test CA"
ca other-ca "mod_audio_fork unrelated CA"

leaf server localhost \
  "subjectAltName=DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth"

# Deliberately carries neither localhost nor 127.0.0.1, so a client that checks
# the peer name against the address it dialled must reject it.
leaf server-othername other.example \
  "subjectAltName=DNS:other.example
extendedKeyUsage=serverAuth"

leaf client "mod_audio_fork test client" \
  "extendedKeyUsage=clientAuth"

rm -f ca.srl
