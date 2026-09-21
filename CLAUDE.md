# mod_audio_fork

Bidirectional FreeSWITCH audio-fork module (WebSocket streaming + playback),
built as a stability-focused replacement for drachtio's mod_audio_fork.
Target: ~1,000 concurrent calls per box.

## Governing documents

- @CONSTITUTION.md — binding coding rules (ownership, comments, singletons,
  concurrency, testing). Read before writing any code; it wins over habit.
- `DESIGN.md` — the locked architecture: decision log, threading model,
  session state machine, memory design, wire protocol, test strategy. Read
  the relevant section before changing anything architectural; propose an
  amendment to it rather than silently diverging.

## Status

M1–M4 (DESIGN.md §12) are implemented, unit/interleaving/sanitizer tested, and
verified on a real FreeSWITCH 1.10.12 through the 50-call rig (`rig/run_smoke.sh`,
CI job `rig-smoke`): fork path, playback round-trip, graceful bye, TLS/mTLS,
`send_text`, DTMF forwarding, `pause`/`resume`/`modify`, memory-cap degradation.
Packaging (one exported symbol, SBOM, Renovate) is in. M5's nightly load tier,
chaos matrix and 48h soak gate are in progress under `rig/load/` and `rig/chaos/`.

## Layout contract

- `core/` — all logic; no FreeSWITCH headers, no globals, constructor
  injection only. This is where every testable line lives.
- `net/` — libwebsockets RAII shim.
- `module/` — thin FreeSWITCH shell (~500 lines); glue only.
- `rig/` — SIPp + mock WS server + toxiproxy integration/load rig.

## Working rules for sessions

- Tests land in the same PR as the code they cover — never as a follow-up.
- Run ASan/UBSan on the suite before calling work done; TSan additionally
  for anything touching threads or atomics.
- Comment policy is the `comment-style` skill; default is zero comments.
- Build/test commands (all from the repo root):
  - `cmake --preset asan && cmake --build --preset asan --parallel && ctest --preset asan`
  - the same three with `tsan` (the preset supplies `tsan.supp`)
  - format: `git ls-files '*.hpp' '*.cpp' | xargs clang-format --dry-run --Werror`
  - tidy: `cmake --preset debug && run-clang-tidy -p build/debug -quiet 'core/(src|tests)/.*\.cpp'`
  - fuzz: `CC=clang CXX=clang++ cmake --preset fuzz && cmake --build --preset fuzz --parallel --target protocol_fuzz && ./build/fuzz/core/tests/protocol_fuzz -runs=200000 -max_len=4096 -timeout=10 /tmp/fuzz-scratch core/tests/fuzz/corpus`
  - module against real FreeSWITCH headers: `docker build --target build .`
