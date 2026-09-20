# mod_audio_fork — Design Document

A FreeSWITCH module that forks call audio to a WebSocket server and plays audio
received back into the call (bidirectional). Functional successor to drachtio's
`mod_audio_fork`, designed specifically to eliminate its instability classes:
session-lifetime races, unbounded buffering, leaky error paths, and shared-thread
stalls.

**Status:** design locked 2026-08-04 (20 decisions below). M1–M2 complete; M3–M4
implemented but not verified on a live call (see README Status).

Two clarifications the implementation forced:
- Playback flow control counts the jitter buffer **and** the media-thread handoff
  ring. Watching only the buffer let total buffering overshoot the configured
  ceiling without ever pausing the socket.
- A `mark` event fires when its audio is handed to the media thread, not when the
  last sample leaves the speaker: carrying per-byte metadata through a lock-free
  ring would cost more than the accuracy is worth. The error is bounded by the
  handoff ring, i.e. `coalesce-max-ms`.

---

## 1. Requirements

| Requirement | Value |
|---|---|
| Language | C++17, `extern "C"` module boundary, no exceptions across FS callbacks |
| Scope | Bidirectional: fork audio out + play server audio into the call |
| Scale target | ~1,000 concurrent calls per box (design headroom to 2,000+) |
| Audio rates | 16kHz L16 PCM primary; other rates (8k, …) configurable per fork; module resamples |
| Wire format | Audio = raw binary WS frames; control/events = JSON text frames only |
| Forks per call | Multiple (configurable cap) |
| Send buffering | Max 10 seconds per fork, drop-oldest |
| Reconnect | Automatic, unbounded with capped backoff |
| Compatibility | Drachtio `uuid_audio_fork` command/event surface + extensions |
| Platform | Linux only; containerized; vanilla FreeSWITCH 1.10.x |

## 2. Decision log

| # | Decision | Choice |
|---|---|---|
| 1 | Language | C++17 (RAII kills leak classes; C ABI boundary) |
| 2 | Scope | Bidirectional core from day one |
| 3 | WS library | libwebsockets, one context per shard, RAII shim |
| 4 | Threading | K ≈ core-count shard threads + lock-free SPSC rings |
| 5 | Process boundary | Everything in-process in the module |
| 6 | Playback injection | Media bug WRITE_REPLACE + adaptive jitter buffer |
| 7 | Session lifetime | Refcounted session + explicit atomic state machine |
| 8 | Buffer memory | 64KB slabs from a global pool, per-fork caps |
| 9 | Reconnect policy | Unbounded, backoff 250ms→5s, keep buffering through gap |
| 10 | Overload protection | Global pool cap + graceful degradation (never OOM) |
| 11 | Framing | Raw PCM binary frames; JSON text for all control |
| 12 | Topology | One WS connection per fork (full duplex) |
| 13 | Playback flow control | Watermark rx pause + `clear`/`mark` barge-in semantics |
| 14 | Control surface | Drachtio-compatible commands/events + extensions |
| 15 | Test architecture | FS-free core library + thin module shell (hexagonal) |
| 16 | Race verification | ASan/UBSan always, TSan stress, exhaustive interleaving enum, libFuzzer on JSON |
| 17 | Integration rig | Docker-compose: FS + SIPp + mock WS server + toxiproxy; PR smoke + nightly leak-asserted load |
| 18 | Release gating | Automated chaos matrix + 48h soak with flat RSS/fd curves |
| 19 | Dependency shipping | libwebsockets vendored + static-linked at a pinned tag; system OpenSSL |
| 20 | Deployment artifact | Multi-stage Docker image on the FS 1.10.x base image |

## 3. Architecture

```
per-call FS media thread (real-time, never blocks)     K shard threads (K ≈ cores)
┌────────────────────────────────┐                     ┌─────────────────────────────┐
│ media bug callback             │   SPSC ring (out)   │ shard N owns:               │
│  READ → push 20ms frame        ├───────────────────► │  • its own lws context      │──► WS server
│  WRITE_REPLACE ← pop from      │ ◄───────────────────┤  • drain rings → WS send    │◄── audio + JSON back
│    jitter buffer               │   SPSC ring (in)    │  • WS recv → jitter buffer  │
└────────────────────────────────┘                     │  • connect/reconnect/close  │
   control plane: uuid_audio_fork commands, FS events   └─────────────────────────────┘
```

Rules that make this safe:

- **Thread affinity:** each fork is assigned to one shard (least-loaded at start)
  and pinned for its lifetime. lws objects are touched **only** on their owning
  shard thread. Cross-thread communication is exclusively SPSC rings + atomics +
  `lws_cancel_service()` wakeups.
- **Media thread contract:** the media bug callback never blocks, never allocates,
  never takes a lock. It copies 20ms frames into/out of preallocated ring slots
  and returns.
- **Blast radius:** a stall on one shard affects 1/K of forks, never all.

### Components

| Component | Lives in | Responsibility |
|---|---|---|
| `Session` (state machine) | core | Lifetime, teardown ordering, entry-point gating |
| `SpscRing` | core | Lock-free frame transport, media↔shard |
| `SlabPool` | core | Global 64KB slab allocator, caps, degradation |
| `JitterBuffer` | core | Playback clocking, watermarks, clear/mark |
| `ProtocolCodec` | core | JSON control message encode/parse (fuzzed) |
| `Backoff` | core | Reconnect schedule with jitter |
| `Resampler` | core | Rate conversion (wraps speexdsp or FS resampler via port) |
| lws shim | net | RAII wrapper over libwebsockets; implements NetPort |
| Module shell | module | FS glue: media bug, commands, events; ~500 lines |

## 4. Session lifetime (Issue 7)

`std::shared_ptr<Session>` with exactly **two strong refs**:

1. Held via the media bug user-data (released in the bug's `SWITCH_ABC_TYPE_CLOSE`).
2. Held by the shard's session registry (released after the WS finishes closing).

Atomic state gates every entry point:

```
CONNECTING → ACTIVE ↔ RECONNECTING
     any state → DRAINING → CLOSING → DEAD
```

- A frame or callback arriving after teardown starts finds `state != ACTIVE` and
  is dropped — never a dangling dereference.
- The destructor calls **zero** FreeSWITCH APIs (safe to run on either thread,
  whenever the last ref drops). All FS resources are released only on the
  bug-CLOSE path.
- Teardown triggers (hangup, `stop` command, server `disconnect`, fatal WS error)
  all funnel through one transition function; every interleaving is enumerable
  in unit tests.

## 5. Memory design (Issues 8, 10)

- **Slabs:** fixed 64KB blocks from a global freelist (mutex-guarded: slab
  acquisition happens on shard/control threads, never on the media path, so
  the uncontended mutex is simpler and TSan-friendlier than lock-freedom that
  buys nothing). Uniform size keeps the pool trivial; duration per slab varies
  with fork rate (64KB = 1s at 16kHz stereo, 4s at 8kHz mono).
- **Per-fork cap:** `10s × bytes/sec` for the send ring (drop-oldest on overflow +
  `overrun` event with gap accounting). Healthy forks hold ~2 slabs; you pay for
  stalls only when they happen. Steady state at 1k calls ≈ 50–100MB, worst case
  bounded.
- **Playback jitter buffer** draws from the same pool; bounded by watermarks
  (§7), not drops.
- **Global cap** (default 1.5GB, configurable): when exhausted, new forks are
  refused with an error event and stalled forks degrade to an emergency 2s cap.
  FreeSWITCH never OOMs; only fork audio quality degrades, observably.
- **APR pools:** small per-call metadata (session struct, names, start metadata)
  lives in the FS session pool — freed automatically with the call. Audio never
  touches APR pools.
- **No naked `new`/`malloc`** outside the pool and shim internals; RAII wrappers
  for lws and cJSON objects so error paths cannot leak.

## 6. Wire protocol (Issues 11–12)

One `wss://` connection per fork, full duplex. Binary frames are bare
interleaved L16 PCM. Text frames are JSON, type-tagged.

### Client → server

| Message | When |
|---|---|
| `{"type":"hello","version":"1.0","callSid":…,"rate":16000,"channels":2,"encoding":"L16","metadata":{…}}` | First frame after connect/reconnect |
| binary PCM | 20ms frames coalesced up to `coalesce_max_ms` (default 100ms) per WS message |
| `{"type":"resume","gapMs":…,"droppedMs":…}` | After reconnect, before backlog drain |
| `{"type":"dtmf","digit":"5","durationMs":160}` | DTMF forwarding |
| arbitrary app JSON | `uuid_audio_fork … send_text` passthrough, verbatim |
| `{"type":"bye"}` | Graceful stop, then WS close handshake |

### Server → client

| Message | Effect |
|---|---|
| binary PCM | Enqueued to jitter buffer → played via WRITE_REPLACE (default: hello rate, mono; override via `start_playback`) |
| `{"type":"start_playback","rate":…,"channels":1}` | Optional playback format declaration |
| `{"type":"clear"}` | Barge-in: flush jitter buffer **and drop incoming audio until next `mark`** (kills in-flight TCP residue) |
| `{"type":"mark","name":…}` | Re-arms playback after `clear`; also emits an FS event when playback reaches the mark |
| `{"type":"disconnect"}` | Server-initiated graceful fork end |
| any other JSON | Relayed to the application as `mod_audio_fork::json` event |

## 7. Backpressure & failure behavior (Issues 9, 13)

| Scenario | Behavior | Observable |
|---|---|---|
| WS peer slow / stalled | Send ring fills to 10s, oldest dropped | `mod_audio_fork::overrun` event with drop stats |
| WS disconnect mid-call | Keep buffering; reconnect with backoff 250ms→5s (jittered), forever until call ends or app stops fork | `::reconnecting`, then `::resume` with gap duration |
| TTS burst (server sends 60s in 2s) | Jitter buffer fills to high watermark (default 10s) → pause socket reads; TCP backpressure holds the rest at the server; resume below low watermark | buffer-depth gauge in `audio_fork status` |
| Caller barge-in | Server sends `clear` → instant flush + drop-until-`mark` | `::playback_cleared` event |
| Global memory cap hit | Refuse new forks; stalled forks degrade to 2s caps | `::start_failed` / `::degraded` events |
| Malformed server JSON | Parse rejected (fuzz-hardened), fork continues, error counted | `::json_error` event |
| Hangup in any state | State machine funnels to DRAINING→CLOSING→DEAD; both refs released; destructor FS-free | counters: live sessions return to 0 |

## 8. Control surface (Issue 14)

Drachtio-compatible verbs so existing dialplans/apps migrate via config change:

```
uuid_audio_fork <uuid> start <wss-url> <mix-type> <sampling-rate> [metadata-json]
uuid_audio_fork <uuid> stop [metadata-json]
uuid_audio_fork <uuid> send_text <json>
uuid_audio_fork <uuid> pause | resume            (extension)
uuid_audio_fork <uuid> modify <wss-url>          (extension)
audio_fork status                                 (extension: JSON counters —
    active forks, buffer depths, pool usage, drops, reconnects, per-shard load)
```

`mix-type`: `mono` (caller), `mixed`, `stereo` (caller/callee split channels).

Events (subclass `mod_audio_fork::`): `connect`, `connect_failed`, `reconnecting`,
`resume`, `overrun`, `degraded`, `playback_start`, `playback_stop`,
`playback_cleared`, `mark`, `json`, `json_error`, `disconnect`, `error`.

## 9. Configuration (`audio_fork.conf.xml`)

| Param | Default |
|---|---|
| `shard-count` | 0 (auto = CPU cores, capped 16) |
| `send-buffer-seconds` | 10 |
| `playback-high-watermark-seconds` / `low` | 10 / 2 |
| `global-memory-cap-mb` | 1536 |
| `emergency-buffer-seconds` | 2 |
| `reconnect-backoff-min-ms` / `max-ms` | 250 / 5000 |
| `coalesce-max-ms` | 100 |
| `max-forks-per-call` | 4 |
| TLS: CA path, client cert/key (mTLS), verify mode | system CA, verify on |

## 10. Testing strategy (Issues 15–18)

**Structure (15):** all logic in `core/` — an FS-free C++ library behind two
narrow ports (`FsPort`: media bug/events/API; `NetPort`: lws shim). The module
`.so` is a thin shell. Anything testable lives in `core/`; logic creeping into
the shell is a review defect.

**Race & memory verification (16):**
- ASan/UBSan on every CI run of the full suite.
- TSan stress: simulated media + shard threads run ~100k randomized teardown
  orderings against the real `Session`.
- Deterministic exhaustive enumeration of state-machine transition interleavings
  (hangup × disconnect × reconnect × stop).
- libFuzzer on `ProtocolCodec` — malformed vendor JSON must never crash FS.

**Integration & load rig (17):** docker-compose — FreeSWITCH + module, SIPp call
generator, controllable mock WS server (bit-exact audio validation, TTS-burst
injection, read stalls, disconnects), toxiproxy for network chaos.
- PR tier: ~50-call smoke with audio-integrity assertions.
- Nightly tier: 1,000 concurrent + 10,000-call churn; **hard leak assertions**:
  RSS back to baseline ±5%, fd count flat, module counters report 0 live sessions.

**Chaos matrix & gating (18):** nightly scripted scenarios — vendor stall,
half-open TCP, 1k-call reconnect storm (backoff jitter verified), global-cap
exhaustion, barge-in flood, JSON fuzz corpus replay, TLS failures,
hangup-during-every-connection-state. Release gate: **48h soak** at ~800
concurrent with continuous churn and flat RSS/fd curves.

## 11. Repository layout

```
├── core/                    # FreeSWITCH-free library (all the logic)
│   ├── include/audiofork/
│   ├── src/                 # session.cpp, spsc_ring.cpp, slab_pool.cpp,
│   │                        # jitter_buffer.cpp, protocol.cpp, backoff.cpp
│   └── tests/               # unit/, tsan_stress/, interleave_enum/, fuzz/
├── net/                     # libwebsockets RAII shim (NetPort impl)
├── module/                  # FS shell: mod_audio_fork.cpp (FsPort impl)
├── rig/                     # docker-compose.yml, sipp/, mock_ws_server/,
│                            # toxiproxy/, chaos/
├── conf/audio_fork.conf.xml
└── DESIGN.md
```

## 12. Milestones

| M | Deliverable | Exit criteria |
|---|---|---|
| M1 | `core/`: state machine, SPSC ring, slab pool + full unit/TSan/enum suite | all interleavings pass under TSan; zero FS deps |
| M2 | `net/` lws shim + `ProtocolCodec` + fuzz harness; echo test vs mock server | 24h fuzz clean; reconnect/backoff verified |
| M3 | Module shell, fork-only path, drachtio-compatible commands; PR smoke rig | 50-call smoke green in CI; drop-in swap works |
| M4 | Playback path: jitter buffer, WRITE_REPLACE, clear/mark, barge-in | bit-exact playback tests; barge-in latency < 1 frame |
| M5 | Chaos matrix, nightly load tier, 48h soak; docs | soak gate green: flat RSS/fd at ~800 concurrent |

## 13. Dependencies & packaging (Issues 19–20)

Linux only. The runtime dependency surface is deliberately minimized to one
library:

**libwebsockets build requirements.** `LWS_MAX_SMP` must be > 1. lws guards its
process-global bookkeeping — the log-context refcount and lifecycle tag list,
both touched by every wsi create and destroy — with a mutex only in SMP-aware
builds. At the default `LWS_MAX_SMP=1` those counters are unguarded, and one
context per shard thread corrupts them: asserting in debug builds, silently
miscounting in release. `LWS_WITH_NETLINK` is also off (Linux route monitoring
we never use, whose POLLIN handler races across contexts). With SMP-aware
locking in place lws guards these itself, so the shim adds no locking of its own
— an outer mutex around context creation inverts lock order against lws's
internal refcount mutex. Note that `mod_verto` also links libwebsockets: two
independently built lws copies in one process are untested here, so avoid
loading both until validated.

**FreeSWITCH on musl requires `session-thread-pool=false`.** FreeSWITCH 1.10.12
built against musl (Alpine) SIGSEGVs during shutdown in
`switch_core_session_thread_pool_worker` (`src/switch_core_session.c:1824`)
whenever any session in the process has carried an `SMBF_WRITE_REPLACE` media
bug — which decision 6's playback path always does. The fault is FreeSWITCH's,
not ours: it reproduces with stock `uuid_displace` and `mod_audio_fork` not
loaded at all. `switch_core_perform_destroy_memory_pool` allocates from the pool
it is destroying, and by then musl's robust-mutex list for that thread points
into unmapped memory. Setting `session-thread-pool=false` in `switch.conf.xml`
gives every session its own thread and removes the faulting path; the rig sets
it and `rig/run_smoke.sh` asserts the FreeSWITCH exit code so a regression
cannot pass unnoticed. Only Alpine/musl aarch64 has been tested; glibc is
unverified and worth checking before the first glibc deployment.

| Dependency | Stage | Strategy |
|---|---|---|
| libwebsockets | runtime | Vendored via CMake FetchContent at a pinned tag; built `-fPIC`, static-linked into `mod_audio_fork.so` with `-fvisibility=hidden` and a version script exporting only the FS module-interface symbol. Identical lws behavior on every host; no symbol collisions with other modules. |
| OpenSSL | runtime | System library, dynamically linked — the same libssl/libcrypto FreeSWITCH already has loaded. Never bundled: two OpenSSLs in one process is a crash source. Distro security updates apply automatically. |
| Resampler | runtime | None shipped. FreeSWITCH core's `switch_resample_*` (Speex-based) implements the core `Resampler` port; core tests use a test implementation. |
| JSON | compile-time | nlohmann/json, header-only, vendored at a pinned version, compiled into the `.so`. Control-plane only. |
| GoogleTest, libFuzzer | dev | FetchContent, pinned. Never in the artifact. |
| SIPp, toxiproxy, mock WS server | rig | Containers in `rig/`. Never in the artifact. |

**Artifact:** multi-stage Dockerfile. Builder stage: FS 1.10.x dev headers +
toolchain, compiles module and vendored lws. Final stage: the FS 1.10.x base
image + `mod_audio_fork.so` + `audio_fork.conf.xml`. Build image = run image,
so glibc compatibility is a non-issue.

**CVE ownership:** static-linking lws means we own its patch cadence.
Automated dependency-pin bumps (Renovate/Dependabot) open a PR; the CI rig
(§10) is the verification that a bump is safe. Target: pin bump to release
within days, not weeks. Security-labeled bumps merge within 48h; routine
bumps batch monthly.

**SBOM (required):** statically linked libraries are invisible to
package-database scanners (Trivy et al. would report the image clean while
vulnerable lws code runs inside the `.so`). The image build emits an SBOM
declaring the vendored lws and nlohmann/json versions so vulnerability
scanning sees them.
