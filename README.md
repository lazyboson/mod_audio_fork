# mod_audio_fork

> ## 🚧 UNDER CONSTRUCTION 🚧
>
> **This module is not finished and must not be used in production.** It is being
> built milestone by milestone (see [Status](#status)); the nightly load tier,
> the chaos matrix, and the 48h soak release gate are still outstanding. APIs,
> wire protocol, and configuration may change without notice until M5 lands.

Bidirectional FreeSWITCH audio-fork module over WebSockets: streams call audio
to a WS server and plays returned audio into the call. A stability-focused
replacement for drachtio's `mod_audio_fork`, designed for ~1,000 concurrent
calls per box.

- [`DESIGN.md`](DESIGN.md) — the locked architecture (20 decisions) and milestones.
- [`CONSTITUTION.md`](CONSTITUTION.md) — binding coding rules.

## Build & test

```sh
cmake --preset asan && cmake --build --preset asan --parallel && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan --parallel && ctest --preset tsan
```

Requires CMake ≥ 3.25 and a C++17 compiler. `core/` and `net/` have no
FreeSWITCH dependency and build anywhere.

The module itself is built only where FreeSWITCH headers are present (detected
via pkg-config, or forced with `-DAUDIOFORK_BUILD_MODULE=ON`):

```sh
cmake --preset release && cmake --build --preset release --target mod_audio_fork
```

Install `build/release/module/mod_audio_fork.so` into FreeSWITCH's module
directory and `conf/audio_fork.conf.xml` into `conf/autoload_configs/`.

On musl builds of FreeSWITCH (Alpine), also set
`<param name="session-thread-pool" value="false"/>` in `switch.conf.xml`:
without it FreeSWITCH 1.10.12 SIGSEGVs at shutdown once any session has carried
a playback media bug (DESIGN.md §13).

## Packaging

`docker build .` produces a two-stage image: an Alpine builder carrying the
FreeSWITCH headers and toolchain, and a runtime stage carrying an Alpine
FreeSWITCH plus `mod_audio_fork.so` in `/usr/lib/freeswitch/mod/` and
`audio_fork.conf.xml` in `/etc/freeswitch/autoload_configs/`. Nothing else
ships.

libwebsockets is vendored and statically linked, and carries one local patch —
[`net/patch_lws_client_http.cmake`](net/patch_lws_client_http.cmake), a
use-after-free reachable when the HTTP upgrade request fails to write.
nlohmann/json is header-only and compiled in. Neither is visible to a
package-database scanner, so the image build writes a CycloneDX 1.5 SBOM
naming both to `/usr/share/doc/mod_audio_fork/sbom.cdx.json`; their versions
are read back out of the CMake pins, so the SBOM cannot drift from what was
built. OpenSSL is not bundled — the module links `libssl`/`libcrypto`
dynamically and shares the copy FreeSWITCH already loaded, and the SBOM names
the image's package version for it. The `.so` exports exactly one symbol,
`mod_audio_fork_module_interface`.

```sh
docker run --rm <image> cat /usr/share/doc/mod_audio_fork/sbom.cdx.json
```

| Build ARG | Default | Meaning |
|---|---|---|
| `AUDIOFORK_VERSION` | `0.0.0-dev` | Names the build in the SBOM and in `org.opencontainers.image.version`. Pass `$(git describe --tags --always)`. |
| `AUDIOFORK_STRIP` | `1` | `0` builds RelWithDebInfo and skips the strip, so a crash in a rig container has a symbolised backtrace. |

## Using it

```
uuid_audio_fork <uuid> start ws[s]://host[:port]/path <mono|mixed|stereo> <rate> [metadata-json]
uuid_audio_fork <uuid> stop
uuid_audio_fork <uuid> send_text <json>
uuid_audio_fork <uuid> pause
uuid_audio_fork <uuid> resume
uuid_audio_fork <uuid> modify ws[s]://host[:port]/path
audio_fork status          # JSON counters, see below
```

Every verb applies to all forks on the channel and answers `-ERR no fork
running on this channel` when there are none.

`send_text` takes everything after the verb as its payload, so the JSON may
contain spaces; it is validated as JSON and then relayed to every fork on the
channel byte for byte. Text sent before the socket is up is queued (64
messages, drop-oldest) and flushed straight after the `hello`.

`pause` stops the outbound audio at the media thread and `resume` starts it
again; both are idempotent. The socket stays connected and the server is told
nothing — there is no pause message in the wire protocol — and playback from
the server keeps reaching the caller throughout.

`modify` repoints a live fork at a different server: the current one gets a
`bye` and a graceful close, and the fork reconnects to the new URL carrying
whatever audio it had buffered. The new server receives a `hello` and no
`resume`, because it never saw the stream those gap and drop totals describe.

Caller DTMF is forwarded automatically — no command needed — to every fork on
the channel as `{"type":"dtmf","digit":"5","durationMs":160}`.

`audio_fork status` answers one JSON object: `calls`, `forks`, `shards` and
`shard_load` (forks per shard); the outbound counters `sent_bytes`,
`media_dropped_bytes`, `buffer_dropped_bytes`, `buffered_bytes` and
`buffered_bytes_max` (the deepest single fork); the playback counters
`playback_bytes_played`, `playback_bytes_dropped`, `playback_buffered_bytes`
and `playback_buffered_bytes_max`; `reconnects`, `barge_ins` and
`pending_texts_dropped`; the overload counters `degraded_forks`,
`paused_forks` and `start_failed`; and the pool gauges `pool_allocated_bytes`,
`pool_leased_slabs`, `pool_cap_bytes` and `pool_slab_bytes`.

When the global slab pool is exhausted, a new fork is refused with `-ERR
global memory cap reached` and a `start_failed` event, and forks that are
still buffering cut back to `emergency-buffer-seconds` of audio, fire
`degraded` once for the episode, and return to the full send buffer once the
pool recovers and their buffer drains.

## TLS

`wss://` URLs are TLS, defaulting to port 443, and are configured once for the
whole module in `audio_fork.conf.xml` — there is no per-fork override:

| Param | Meaning |
|---|---|
| `tls-ca-file` | PEM file of CAs to trust. Empty uses the OS trust store. A file, not a directory — libwebsockets offers no CA-directory option. |
| `tls-cert-file` | Client certificate (PEM) for mutual TLS. |
| `tls-key-file` | Its private key (PEM). |
| `tls-verify` | `true` by default. `false` accepts any server certificate — self-signed, expired, wrong hostname — and is for test rigs only; the module logs a warning at load. |

For mutual TLS set `tls-cert-file` and `tls-key-file` together; with only one of
the two the module logs an error and connects without a client certificate. A
failed handshake surfaces exactly like a refused connection: a `connect_failed`
event followed by the usual reconnect backoff.

Events are fired as custom events with subclass `mod_audio_fork::<name>`
(`connect`, `reconnecting`, `resume`, `overrun`, `degraded`, `start_failed`,
`json`, `stop`, `playback_start`, `playback_cleared`, `mark`, …).

The server drives playback over the same socket: binary frames are PCM to put in
the caller's ear, `{"type":"clear"}` is barge-in (flushes buffered audio and
mutes until the next `{"type":"mark","name":…}`), and `{"type":"start_playback",
"rate":…}` declares a rate other than the fork's.

## Status

- M1 (core foundation) ✅ — session state machine, SPSC ring, slab pool, with
  unit, exhaustive-interleaving, and sanitizer test suites.
- M2 (network shim + protocol) ✅ — libwebsockets RAII event loop (`net/`),
  wire-protocol codec with libFuzzer harness, jittered reconnect backoff,
  TLS/mTLS with system-CA verification, a patched lws use-after-free.
- M3 (module shell + fork path) ✅ — sharded runtime, the drachtio-compatible
  verbs plus `pause`/`resume`/`modify`, DTMF forwarding, `audio_fork status`
  JSON. Verified on FreeSWITCH 1.10.12 by the 50-call smoke (`rig/run_smoke.sh`,
  CI job `rig-smoke`): hello/audio/bye per call, counters back to zero,
  FreeSWITCH exits 0.
- M4 (playback) ✅ — jitter buffer, WRITE_REPLACE injection with rate
  conversion, watermark flow control, `clear`/`mark` barge-in; the smoke
  asserts server audio reaches the caller byte-for-byte.
- Memory design (DESIGN.md §5) ✅ — global cap refuses new forks
  (`start_failed`) and degrades stalled ones to the emergency cap (`degraded`).
- Packaging (DESIGN.md §13) ✅ — one exported symbol, CycloneDX SBOM in the
  image, Renovate pin bumps.
- M5 (load, chaos, soak) — **in progress**: nightly SIPp load tier with leak
  assertions, toxiproxy chaos matrix, and the 48h soak gate live under
  `rig/load/` and `rig/chaos/`. Until the soak gate is green, do not deploy —
  see the banner above.

## License

[MIT](LICENSE).
