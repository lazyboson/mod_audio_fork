#!/bin/sh
# Writes a CycloneDX 1.5 SBOM for the runtime image to stdout. libwebsockets and
# nlohmann/json are statically linked, so no package-database scanner can see
# them (DESIGN.md §13); their versions are read back out of the CMake
# FetchContent pins so a pin bump cannot leave this file behind.
set -eu

PINS=${1:?usage: sbom.sh <tree containing net/CMakeLists.txt and core/CMakeLists.txt>}
VERSION=${AUDIOFORK_VERSION:-0.0.0-dev}

require() {
  [ -n "$2" ] || { echo "sbom: could not read $1" >&2; exit 1; }
  case "$2" in
    *[!A-Za-z0-9._+~/:@-]*)
      echo "sbom: $1 holds characters this generator cannot emit safely: $2" >&2
      exit 1
      ;;
  esac
}

declare_block() {
  awk -v head="FetchContent_Declare($2" '
    index($0, head) == 1 { inside = 1 }
    inside { print }
    inside && /^\)/ { exit }
  ' "$1"
}

block_url() {
  printf '%s\n' "$1" | sed -n 's/^[[:space:]]*URL[[:space:]][[:space:]]*\(http[^[:space:]]*\).*/\1/p'
}

block_sha256() {
  printf '%s\n' "$1" |
    sed -n 's/^[[:space:]]*URL_HASH[[:space:]][[:space:]]*SHA256=\([0-9a-f]\{64\}\).*/\1/p'
}

url_tag() {
  printf '%s\n' "$1" | sed -n \
    -e 's#.*/archive/refs/tags/\([^/]*\)\.tar\..*#\1#p' \
    -e 's#.*/releases/download/\([^/]*\)/.*#\1#p'
}

lws=$(declare_block "$PINS/net/CMakeLists.txt" libwebsockets)
lws_tag=$(url_tag "$(block_url "$lws")")
lws_sha=$(block_sha256 "$lws")
require "the libwebsockets tag" "$lws_tag"
require "the libwebsockets SHA256" "$lws_sha"

json=$(declare_block "$PINS/core/CMakeLists.txt" nlohmann_json)
json_tag=$(url_tag "$(block_url "$json")")
json_sha=$(block_sha256 "$json")
require "the nlohmann/json tag" "$json_tag"
require "the nlohmann/json SHA256" "$json_sha"

# The module links whatever libssl the image resolves at dlopen time, so ask apk
# which package owns it rather than naming one.
ssl_so=$(ls /usr/lib/libssl.so.* 2>/dev/null | head -n 1 || true)
require "the installed libssl path" "$ssl_so"
ssl_pkg=$(apk info -W "$ssl_so" 2>/dev/null | sed -n 's/.*owned by //p')
require "the apk package owning $ssl_so" "$ssl_pkg"
# apk prints name-version-rREVISION
ssl_name=${ssl_pkg%-*-*}
ssl_version=${ssl_pkg#"$ssl_name"-}
require "the OpenSSL package name" "$ssl_name"
require "the OpenSSL package version" "$ssl_version"

require "the mod_audio_fork version" "$VERSION"

cat <<JSON
{
  "bomFormat": "CycloneDX",
  "specVersion": "1.5",
  "version": 1,
  "metadata": {
    "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
    "component": {
      "bom-ref": "mod_audio_fork",
      "type": "application",
      "name": "mod_audio_fork",
      "version": "$VERSION"
    }
  },
  "components": [
    {
      "bom-ref": "libwebsockets",
      "type": "library",
      "name": "libwebsockets",
      "version": "${lws_tag#v}",
      "purl": "pkg:github/warmcat/libwebsockets@$lws_tag",
      "scope": "required",
      "description": "Statically linked into mod_audio_fork.so and therefore invisible to package-database scanners.",
      "hashes": [
        { "alg": "SHA-256", "content": "$lws_sha" }
      ],
      "pedigree": {
        "patches": [
          {
            "type": "unofficial",
            "resolves": [
              {
                "type": "security",
                "name": "lws-client-http-write-failure-use-after-free",
                "description": "Applied by net/patch_lws_client_http.cmake: lws_http_client_socket_service() frees the wsi when the HTTP upgrade request fails to write and then returns 0, so rops_handle_POLLIN_h1() dereferences freed memory. Reachable from a TLS server that rejects our client certificate."
              }
            ]
          }
        ]
      }
    },
    {
      "bom-ref": "nlohmann-json",
      "type": "library",
      "name": "nlohmann/json",
      "version": "${json_tag#v}",
      "purl": "pkg:github/nlohmann/json@$json_tag",
      "scope": "required",
      "description": "Header-only, compiled into mod_audio_fork.so and therefore invisible to package-database scanners.",
      "hashes": [
        { "alg": "SHA-256", "content": "$json_sha" }
      ]
    },
    {
      "bom-ref": "openssl",
      "type": "library",
      "name": "$ssl_name",
      "version": "$ssl_version",
      "purl": "pkg:apk/alpine/$ssl_name@$ssl_version",
      "scope": "required",
      "description": "System OpenSSL, resolved dynamically at dlopen time so the module shares the copy FreeSWITCH already loaded. Never bundled (DESIGN.md §13)."
    }
  ],
  "dependencies": [
    { "ref": "mod_audio_fork", "dependsOn": ["libwebsockets", "nlohmann-json", "openssl"] },
    { "ref": "libwebsockets", "dependsOn": ["openssl"] },
    { "ref": "nlohmann-json", "dependsOn": [] },
    { "ref": "openssl", "dependsOn": [] }
  ]
}
JSON
