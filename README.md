# mod_audio_fork

> ## 🚧 UNDER CONSTRUCTION 🚧
>
> **This module is not finished and must not be used in production.** It is being
> built milestone by milestone (see [Status](#status)); playback, the load rig,
> and the soak-test release gate are still outstanding. APIs, wire protocol, and
> configuration may change without notice until M5 lands.

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

## Using it

```
uuid_audio_fork <uuid> start ws://host:port/path <mono|mixed|stereo> <rate> [metadata-json]
uuid_audio_fork <uuid> stop
uuid_audio_fork <uuid> send_text <json>
audio_fork status          # JSON counters: forks, bytes, drops, reconnects, pool use
```

`send_text` takes everything after the verb as its payload, so the JSON may
contain spaces; it is validated as JSON and then relayed to every fork on the
channel byte for byte. Text sent before the socket is up is queued (64
messages, drop-oldest) and flushed straight after the `hello`.

Caller DTMF is forwarded automatically — no command needed — to every fork on
the channel as `{"type":"dtmf","digit":"5","durationMs":160}`.

Events are fired as custom events with subclass `mod_audio_fork::<name>`
(`connect`, `reconnecting`, `resume`, `overrun`, `json`, `stop`,
`playback_start`, `playback_cleared`, `mark`, …).

The server drives playback over the same socket: binary frames are PCM to put in
the caller's ear, `{"type":"clear"}` is barge-in (flushes buffered audio and
mutes until the next `{"type":"mark","name":…}`), and `{"type":"start_playback",
"rate":…}` declares a rate other than the fork's.

## Status

- M1 (core foundation) ✅ — session state machine, SPSC ring, slab pool, with
  unit, exhaustive-interleaving, and sanitizer test suites.
- M2 (network shim + protocol) ✅ — libwebsockets RAII event loop (`net/`),
  wire-protocol codec with libFuzzer harness, jittered reconnect backoff,
  echo/reconnect integration tests against an in-process mock WS server.
- M3 (module shell + fork path) — **partially complete.** The fork path,
  sharded runtime, and the full drachtio-compatible command surface (`start`,
  `stop`, `send_text`, `audio_fork status`) plus automatic DTMF forwarding are
  implemented and covered by tests green under ASan/UBSan and TSan.

  Verified against a real FreeSWITCH 1.10.12: the module compiles against its
  headers, loads and unloads cleanly, reads `audio_fork.conf.xml`, and answers
  `uuid_audio_fork` / `audio_fork status`.

  **Not yet verified:** audio flowing through a live call, and the 50-call
  smoke run (`rig/`). Those close out with the load rig in M5. Do not deploy
  this yet — see the banner above.
- M4 (playback) — **implemented, same verification gap as M3.** Jitter buffer,
  WRITE_REPLACE injection with rate conversion, watermark flow control, and
  `clear`/`mark` barge-in. 139 tests green under both sanitizers, including
  byte-exact playback, barge-in, and app-text/DTMF passthrough over real
  sockets. Playback has never been heard on a live call, and no DTMF digit has
  ever been forwarded from one — that needs the rig.
- M5 (chaos + soak) — not started: rig completion, nightly load tier, 48h gate.
