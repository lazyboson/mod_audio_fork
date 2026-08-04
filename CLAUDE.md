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

Design phase complete; no code yet. Next milestone is M1 (DESIGN.md §12):
`core/` library — session state machine, SPSC ring, slab pool — with full
unit/TSan/interleaving test suites and zero FreeSWITCH dependencies.

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
- Build/test commands: to be added when M1 lands the build system — update
  this section in that PR.
