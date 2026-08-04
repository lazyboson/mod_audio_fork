# mod_audio_fork

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

Requires CMake ≥ 3.25 and a C++17 compiler. `core/` has no FreeSWITCH
dependency and builds anywhere; the FreeSWITCH module shell arrives in M3.

## Status

M1 (core foundation) — session state machine, SPSC ring, slab pool, with unit,
exhaustive-interleaving, and sanitizer test suites.
