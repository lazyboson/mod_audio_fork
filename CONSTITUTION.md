# Constitution

Governing rules for all code in this repository. `DESIGN.md` says *what* we
build; this document says *how*. Where they conflict, this document wins, and
the conflict is a defect to be resolved by amending one of them — never by
ignoring either. Rules change only by editing this file in a reviewed PR.

## Article 1 — Language & standard

1. C++17. Modern practice per the C++ Core Guidelines; where a rule below is
   stricter, the rule below wins.
2. Warnings are errors (`-Wall -Wextra -Werror`). `clang-format` and
   `clang-tidy` run in CI; a diff that fails either does not merge.
3. `const` by default; `enum class` over enum; `[[nodiscard]]` on anything
   returning a status or resource; no macros where a function, template, or
   `constexpr` works.

## Article 2 — Ownership: RAII only

1. Every resource — heap memory, sockets, lws handles, FreeSWITCH refs, file
   descriptors, slab leases — is owned by exactly one RAII type. No resource
   is acquired outside a constructor/factory or released outside a destructor.
2. No naked `new`/`delete`/`malloc`/`free`. The only exemption is the
   internals of `SlabPool`, which exists to own that complexity.
3. **No owning raw pointers, anywhere.** `std::unique_ptr` is the default
   owner; `std::shared_ptr` only where the design explicitly requires shared
   lifetime (the Session two-ref model in DESIGN.md §4) — each use cites the
   design section.
4. Non-owning access is by reference, or by a named view type (span/string
   view). Raw non-owning pointers are permitted **only** in `module/` and
   `net/` at the C ABI boundary (FreeSWITCH callbacks, libwebsockets), and
   must be converted to an owned or reference form in the same function they
   are received. `core/` contains no raw pointers at all.
5. Rule of zero: types own resources via members, not hand-written special
   members. A hand-written destructor is a design smell requiring
   justification in review.

## Article 3 — Comments

1. The comment policy is the `comment-style` standard: **a comment must state
   something the code cannot.** If deleting it loses nothing for a competent
   reader, it must not exist.
2. Permitted comments state invisible constraints (ordering, lifetime,
   memory-ordering rationale on atomics), external-system quirks
   (FreeSWITCH/lws API behavior worked around), units and sentinels, or
   deliberate negative space. Nothing else — no narration, no section
   banners, no restating the next line, no commented-out code, no TODOs
   without an issue link.
3. The default state of a file is zero comments. Readability comes from
   names, types, and small functions; if a comment merely helps someone read
   faster, improve the name instead.

## Article 4 — Singletons & dependency injection

1. Singleton state is confined to the composition root: the module shell owns
   exactly one instance each of `Config`, `SlabPool`, and `ShardManager`,
   constructed in module load and destroyed in module shutdown, in reverse
   order of construction.
2. Because FreeSWITCH modules unload and reload, singleton lifetime is
   explicit — owned by the load/unload path. Function-local static singletons
   (Meyers pattern) are forbidden: their destruction order at unload is
   uncontrolled.
3. `core/` has no globals and no singleton access. Every core type receives
   its dependencies through its constructor. If a core type reaches for a
   global, it is untestable and the change is rejected.

## Article 5 — Concurrency

1. The media-bug path never blocks, never allocates, never takes a lock, and
   never throws. It copies frames through preallocated SPSC rings and returns.
2. libwebsockets objects are touched only on their owning shard thread.
   Cross-thread signaling is SPSC rings, atomics, and `lws_cancel_service()`.
3. Every atomic carries an explicit memory ordering, and each
   non-`seq_cst` ordering carries a comment stating why it is sufficient
   (an Article 3 invisible constraint).
4. Session entry points are gated by the state machine (DESIGN.md §4); no
   code path touches session resources without first loading the state.

## Article 6 — Errors & boundaries

1. No exception crosses the C ABI (FreeSWITCH callbacks, lws callbacks,
   module entry points). Boundary functions are `noexcept` shells that
   translate results to status codes.
2. Fallible construction uses factories returning a result type; hot paths
   are exception-free by construction.
3. Every error path releases what it acquired — which Article 2 makes
   automatic. An error path that needs manual cleanup is misdesigned.

## Article 7 — Testing

1. Untested code does not merge. Every `core/` component ships with unit
   tests in the same PR; concurrency-bearing code ships with TSan stress or
   interleaving-enumeration coverage (DESIGN.md §10).
2. ASan/UBSan green is a merge gate; TSan green is a merge gate for anything
   touching threads or atomics.
3. `core/` includes no FreeSWITCH headers — enforced by the build, not by
   convention. Logic found in `module/` beyond thin glue is a review defect.
