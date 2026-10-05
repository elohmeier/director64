# Roadmap: a converter-first Director64

The complementary [browser and local ISO roadmap](web-roadmap.md) covers a
Wasm backend, shared Rust import/conversion, portable game packages, playability
evidence and GitHub Pages hosting. Its milestones build on this converter work.

Status as of 2026-09-26. This document records why the port is being
re-architected around the converter, what the target looks like, the
decisions already taken, and the milestones with the gate each one has to
pass. Update the status table at the end when a milestone lands.

## Why

Director64 already converts: the device never parses Director files or
Lingo. Scores and casts are const tables, and Lingo is lowered on the host.
What the device still does at run time is bind *meaning by name*. Every
generated op is a runtime call that receives an interned string and walks
compare chains to find out what it is (`lv_binary` tests `=`, `<>`, `and`,
`or` and the string operators before it reaches `+`; the property getter in
`runtime/director/director.c` is a 222-link chain). The nine optimization
rounds between 2026-09-12 and 2026-09-26 were, almost without exception,
caches for facts the converter already knew: handler lookup, method
resolution, member-name index, property memo, mouse-handler memo, event
quiet bits, film-loop memo, symbol interning. The profile is now flat, which
is the signature of a design-level overhead rather than a hot spot.

Measured state at the start of this roadmap:

| Cost | Measured |
| --- | --- |
| Lingo step on the VR4300 | 53 to 294 µs per statement, roughly 5k to 28k cycles |
| Name-compare sites | 294 in director.c, 157 in lingo_runtime.c, 139 in d10.inc |
| Version guards | 207 `#if` in director.c, 157 in director_main.c |
| Value and runtime layout | 16 bytes per value; about 1 MB `lv_runtime_t` against an 8 KiB direct-mapped D-cache |
| Image decompression | 0.7 to 1.3 MB/s at level 3, 3.4 to 5.3 MB/s at level 2 |
| Image files, Workshop | 3,976 separate files, each opened and decompressed on a cache miss |
| Palettized sources stored at 16 bpp | Workshop 99.7% of pixels, Löwenzahn 100% |
| Code overlays, Deutsch | 5.0 MB total, single casts of 444 KB, about 2.9 MB resident |

Per-port ceilings live in each port's pacing budget
(`max_service_us_per_step`) and are the yardstick for Track A.

## Target

The converter does the semantic binding the runtime does today and emits
data the runtime executes without interpreting names.

- **Typed IR with static resolution.** Handler calls become indices into a
  per-movie import table resolved once at overlay load. `the mouseH`,
  `sprite(n).locH`, member and cast references, globals and behavior
  properties resolve to slot numbers or struct offsets. Operators lower to
  typed ops with a tag switch on operand types. Script classes get fixed
  property layouts. Symbols become 16-bit ids across the whole corpus.
- **Precomputed engine tables.** Event eligibility per member and channel,
  draw-order candidates, film-loop facts and mouse-handler presence are
  static per movie and are emitted instead of memoized.
- **Bytecode plus tables, not C.** Compact bytecode with resolved operands
  runs the same statement in fewer cycles than a native state machine that
  pays a call per op, shrinks overlays to a fraction of today's size, removes
  dlopen relocation cost, and decouples the converter from the device
  language and toolchain. The same bytes run on the host probe.
- **A small evaluator stays on device** for `do` and `value(` (present in
  five of six ports, up to 118 sites in Deutsch), or converter-time lowering
  where the argument is a literal.
- **Per-scene asset bundles.** The score says which members each frame
  shows and the perf state walk records the empirical working set per
  movie. One contiguous bundle per scene replaces the fopen-per-image path,
  the LRU thrash and the arena-fragmentation class of bugs.
- **Texture formats from the source, not one format for all.** CI8 or CI4
  for palettized sources halves or quarters decompressed bytes on four of
  the six ports. Codec tier, colour bits, resampling and prescale become
  inputs to a per-scene budget solver against the 56 MiB ROM ceiling.
- **Runtime split by Director family** (D5/D6 and D7+/D10) or
  converter-generated version-specific parts, replacing the preprocessor
  guards and the "feature never widened" failure mode.

What stays on device is the engine: score playback, sprite and event
semantics, ink compositing, audio, a compacting GC. What must survive every
milestone is the qualification harness: captures with pacing gates, the
fuzzer with its seed baseline, the journeys, the native probes and the
per-port budgets. It is the correctness oracle for the whole roadmap.

## Decisions

1. **Emit bytecode and tables, not C.** The converter's output is a data
   contract. The runtime language can change later without touching it.
2. **Parity before speed.** Every Track A milestone first reproduces the
   previous milestone's probe state stream byte for byte under the fuzzer's
   command streams (`director64 parity`), then claims its measured gain.
3. **Rust on the host, not on the device.** The new IR and compiler stage is
   the place where a typed language pays off and may be written in Rust
   once the IR exists. The device runtime stays C: Rust's MIPS targets are
   tier 3, the N64 target is a community JSON spec on nightly with
   `build-std`, the LLVM MIPS backend only gained maintainers in September
   2026, and libdragon-rs is bindings over the same C SDK. Memory safety on
   device is already covered by the sanitized host probe and the fuzzer.
   Revisit once the bytecode contract has removed C generation entirely.
4. **One game at a time, plain-D6 Workshop first**, because it has the
   smallest semantics, then Willy (extended D6), Christmas (D7), Mucklas
   (D8), Löwenzahn (D5) and Deutsch (D10).
5. **Keep ProjectorRays and the media decoders where they are.** They
   connect through the JSON that already exists. Consolidating the host into
   one language is Track C and optional.

## Tracks and milestones

### Track A: scripts

| Milestone | Change | Gate |
| --- | --- | --- |
| A1 Bytecode contract | the compiler emits bytecode per handler; `lv_vm_step` executes it over the existing `lx_*` services with the same state boundaries, so step accounting and GC points are unchanged. Handlers keep a `step` function pointer for hand-written tests. | pytest and test-native pass; `director64 parity` byte-identical on all six probes; overlay bytes and `service_us_per_step` recorded per port |
| A2 Static operators and builtins | One opcode per operator with typed fast paths that reproduce `binary()` exactly; `the X`, property and builtin names become ids into generated dispatch tables; `lv_get`/`get()` chains become switches | parity; per-step µs down on Mucklas station and Willy 10.DXR entry |
| A3 Static names | Per-movie handler import tables resolved at overlay load; corpus-wide symbol ids; script-class property layouts; property lists keyed by symbol id; precomputed event and mouse-handler tables | parity; no string compare on the tick path; memo tables deleted |
| A4 Value model and layout | 8-byte values with boxed doubles; heap and frame stack redesigned for the D-cache; runtime split per Director family | parity; cache-offset tuning no longer load-bearing; RAM headroom on Deutsch |
| A5 Evaluator | Restricted `do`/`value(` evaluator on device, converter-time lowering of literal arguments | parity; fuzz baseline unchanged |

### Track B: assets

| Milestone | Change | Gate |
| --- | --- | --- |
| B1 Source-depth textures | CI8/CI4 planes with TLUTs for palettized sources; matte and mask inks evaluated on indices; FDI keeps the 16-bit path for true-colour sources | boot-gate image checks pass; bytes decompressed per entry halved on Workshop, Löwenzahn, Willy, Christmas |
| B2 Scene bundles | Per-scene working set from the score plus the perf state walk; one bundle per scene, one allocation, released as one block | `NATIVE_IMAGE_COST read_us` and `largest=` at entry; train-soak flat; image region and LRU sweep deleted |
| B3 Budget solver | Per-scene format, tier and rate choices solved against the ROM ceiling and RAM | ROM under 56 MiB on every port; tick delivery per capture unchanged or better |

### Track C: host consolidation

The compiler stage lives in Rust under `compiler/` (`director64-aot`): it
reads the parser's program.json, lowers every handler through a typed AST
and writes the bytecode units, listings, globals and manifest. It began
as a byte-for-byte port of the Python generator, which was retired at the
start of A3 so the bytecode contract has one author; `mise run compiler`
builds and tests it, `src/director64/aot.py` runs it for the pipeline and
the tests. ProjectorRays stays the parser; media conversion stays in Node
until a milestone needs it elsewhere. The Rust stage is where A3's static
binding is built.

## Measurement

Each milestone records, per port, the pacing summary's
`service_us_per_step` for the lockstep captures, the overlay bytes under
`filesystem/code`, the bytes decompressed at scene entry, minimum tick
delivery and dropped service time. Captures are deterministic; compare only
between runs with identical step counts. The host probe is the fast loop
and the emulator is the benchmark.

## Finding 2026-09-26: the console is memory-bound, not dispatch-bound

The station profile on the host and the same scene on the console give the
ratio that decides the order of the remaining work. On the host a Lingo step
costs about 96 ns (13.4 µs per tick over 140 steps); on the console it
costs about 50 µs. That is a factor of 500 in time and about 16 in cycles,
far more than the width and clock of the two CPUs explain. The VR4300's
8 KiB direct-mapped data cache against a 1 MB runtime structure, 16-byte
values and heap-walked property lists means that a step is dominated by
cache misses, and every dispatch-level change lands inside a swing of a few
percent that code placement alone produces (the A2 captures show it: the
same change is −7% on one port and +3% on another).

Consequence: A4 (value model and runtime layout) should come before A3's
call and property caches, because it changes what a step touches, while A3
changes what a step computes. The Rust compiler stage is in place for
either.

## A4 evidence: the first measured layout tier

`tools/cache-profile/` attributes every cache fill of an emulator capture
to a function and a structure field. On the Mucklas train scene the first
profile did not point at the value model at all:

| Data misses | Where | Fix |
| --- | --- | --- |
| 14.5% | `mixer_ch_set_vol`, eight calls per loop pass | hand the mixer a gain only when it changes |
| 14% | audio-buffer polling and `get_ticks_us` per pass | pump audio every 4 ms; raw counter reads in the hot accounting |
| 51% (second profile) | the main loop's own statics and the C stack, aliasing the aligned runtimes on every idle pass | idle passes wait on the counter until the next tick is due |
| 4.6% | `lv_run` reading six scalars a megabyte apart | the per-step scalars are the first lines of `lv_runtime_t` |
| 3.2% | `singleton` walking the script-object table | a validated memo |

Both runtimes now start at fixed offsets into the cache's 8 KiB period so
their layout is deterministic. Result on the train scene: data misses
84 M to 50 M, data accesses 3.4 G to 0.8 G, `service_us_per_step` 50.29
to 48.07, the window's maximum work 3.99 s to 3.86 s, parity clean on all
six ports.

What remained after that was the interpreter's own traffic, and the
largest share, the C stack at 21.6% of data misses, had a structural
cause: `lv_t` was a 16-byte struct and the o64 ABI returns such a struct
through memory, so every value-returning runtime call spilled to the stack.

### A4 evidence: the 8-byte value

`lv_t` is now one 64-bit word. Handles and integers are boxed with a zero
top half (type in bits 32..47, a 32-bit id or integer below), so the
all-zero word is VOID and zeroed memory reads as VOID; a float is its IEEE
double plus 2^49, which no double can produce with a zero top half, with
NaNs canonicalized. Integers are 32-bit as Director's are; an integral
value beyond that range, which no port's corpus contains, becomes a float.
The value returns in a register, the value stack, locals, globals and
lists are half the size, and the runtime shrank from 944 KB to 871 KB. The
conversion of about 1,500 member accesses was driven by the compiler:
the members were removed and each rejected access rewritten by a script,
with the float-flag reads and the assignments converted by hand.

| Mucklas train | before A4 | after tier 1 | after 8-byte values |
| --- | ---: | ---: | ---: |
| µs per Lingo step | 50.29 | 48.07 | 41.98 |
| window maximum work | 3.99 s | 3.86 s | 3.34 s |
| data misses | 84 M | 50 M | 42 M |

Parity: every field of about 6,200 states across the six ports matched
except object-handle numbers stored in globals and properties (twelve
divergences, all of the form 52 became 4283), because collections now
trigger at different heap fill levels and handles are reused in a
different order. Director never exposes handle identity, so the value
formatter now names an instance by its allocation serial (`<instance N>`),
which the program's allocation order fixes and the collector's cadence
cannot move; every object records the serial in every profile. Reference
probes rebuilt with that rendering replay exactly on all six ports (6,257
states, zero divergences), and the instance names appear in the recorded
states of the three ports that hold instances in globals. The two
heap-footprint metrics and the handle count remain volatile for parity.

### A4 evidence: the generated handler index

Every handler lookup walked the movie's handler table comparing names,
behind a 24.5 KiB two-way lookup cache in the value runtime and a 9.7 KiB
method cache in the director; the walk, the caches and the string compares
under them were about 13% of the train's data misses. The converter now
emits an index of each movie's handler table by name (FNV-1a over the
folded name, the same key as the member-name index, `lv_text_hash`): a
lookup reads one bucket, in table order, so it answers exactly what the
walk answered. Both caches are deleted; the call cache stays for the
cross-movie search that A3 resolves at convert time.

| Mucklas train | after 8-byte values | after the handler index |
| --- | ---: | ---: |
| µs per Lingo step | 41.98 | 38.82 |
| window maximum work | 3.34 s | 3.23 s |
| data misses | 42 M | 34 M |
| value runtime | 871 KB | 847 KB |

The index costs two 16-bit entries per handler plus one per bucket in the
overlay (Deutsch's generated C grew 0.8%). Parity against the pre-index
probes: 6,257 states, zero divergences. What remains of lookup is
`lv_scan` itself at 4.9% of data misses (the entry and its name string)
and the call cache at 2.6%; both go with A3's per-site resolution.

### A4 evidence: the C stack

Splitting the data misses by address showed the C stack's top 64 KiB at
18% of them, more than the sprite table. The target compiler's
`-fstack-usage` named the frames: the property getter reserved 2.2 KB (a
zeroed 256-value label array for `the markerList`, memset on every
property read), `invoke` 1.6 KB (a selector buffer and two LV_LOCALS
arrays for `do` and the file objects, on every call), `seek_now` 3.4 KB
(the change masks of 801 channels, zeroed every frame), `hit_sprite`
1.8 KB (the draw order, every tick), and each method call assembled its
receiver-plus-arguments in a 768-byte array only to copy it again into
the frame. Every callee of those functions ran that far down the stack,
and the direct-mapped cache paid for the spread.

The rare branches now own their buffers in `noinline` helpers entered only
when taken, the per-frame and per-tick scratch is resident in the director
runtime, and `push_with` copies a leading receiver straight into the new
frame's locals. The hot frames are now 136 to 248 bytes.

| Mucklas train | after the handler index | after the stack diet |
| --- | ---: | ---: |
| µs per Lingo step | 38.82 | 35.14 |
| window maximum work | 3.23 s | 3.00 s |
| data misses | 34 M | 29 M |
| code misses | 53 M | 45 M |
| stack-region data misses | 6.1 M | 3.9 M |

Data misses by region now: overlays and images 25%, director 24% (the
sprite table two thirds of it), value runtime 24%, static data 14%, stack
13%.

### A4 evidence: the sprite record

The sprite misses concentrate on the hundred-odd occupied channels, and
every line of their 120-byte records missed alike: the fields a pass
reads were spread over the record (type on the second line, loc_z on the
fourth, the puppet flag on the fourth, auto_mask on the eighth). The
score record now keeps loc_z beside type, so the draw order reads one
line per channel instead of two; the sprite's flags follow the record on
its fourth line, with the quad, cursor and constraint after them; and the
frame advance no longer touches a channel's flags when the frame leaves
the channel alone, which is most channels every frame. The score
generator emits the record positionally and moved with it.

| Mucklas train | after the stack diet | after the sprite record |
| --- | ---: | ---: |
| µs per Lingo step | 35.14 | 34.07 |
| window maximum work | 3.00 s | 2.93 s |
| sprite record | 120 B | 112 B |
| director runtime | 353 KB | 335 KB |

The emulator profile dumps at every 4 M data misses and the capture ends
without a final dump, so its miss totals are floors to a multiple of 4 M;
both of these captures fell in the same 28 M band, and the per-step time
is the measure.

What remains in the director is the per-frame pass over all 801 channels
of the score (a line of sprite and a line of score each) and the hit
test's walk of the draw order every tick.

## A3 evidence

### Symbols are ids

A symbol was a heap string: every property lookup walked an instance's
key/value pairs reading each key's object header and text, behind a
memo, and `named_property_index` alone was 12% of the train's data
misses. The compiler now numbers every symbol the corpus spells (symbol
literals, property names, declarations, plus the runtime's own name
vocabulary) into one table per game, `symbols.c`, resident with the
runtime and indexed by the text hash in bucket order; each movie unit
carries the id of every pooled name it reads as a symbol, so a literal is
a constant and a property read binds its id with its name. The runtime
interns what the corpus never spelled (`symbol()`, `value("#x")`, a
name a service looks up) in a bounded resident table behind the
converter's ids. Symbols are values, not objects: equality is id
equality, property keys compare as words over the pairs themselves, the
collector never sees them, and the value heap no longer holds them.

| Mucklas train | after the sprite record | symbols as ids |
| --- | ---: | ---: |
| µs per Lingo step | 34.07 | 33.45 |
| window maximum work | 2.93 s | 2.91 s |
| `named_property_index` data misses | 3.5 M | 1.2 M |

Parity against the pre-symbol probes: four ports exact; Willy and
Deutsch differed only in the spelling of symbols that data spelled with
capitals (`#MacLoColor`, `#fileName`) where the source's lowercase
literal named the symbol first, every other field of every state equal.
Director itself spells a symbol as the first script that named it did;
the parser folded every identifier, so neither side was that. Fixed on
2026-09-27 after Willy's host journey caught it (`#shopFloor` in the
player's junk list read `#shopfloor`): the parser keeps a symbol
literal's spelling (src/director64/lingo.py), the compiler assigns one id
per case-folded spelling and keeps the first spelling as the symbol's
text (compiler/src/emit.rs), name ids fold too, and the parity oracle
compares symbols folded, since their identity is. Parity is exact on all
six ports.

The parity oracle renumbers instances by first appearance in the state
stream (per runtime boot), because the allocation serial the probe prints
moves with every change to what the runtime allocates.

### Calls bound at convert time

Every call statement went through `invoke`: the `do` and constructor
checks, a method resolution on the frame's receiver, a scan of the
movie's handler table for the caller's script, a four-way call cache
keyed by string compares, then the movie-script search across the loaded
movies, and only then the builtin. The compiler now decides three things
per call site. A call the calling script's own handler table answers, or
that a movie script answers when the caller is a movie script or nothing
in the corpus can give an instance an ancestor, binds to that entry
(`LB_INVOKE_LOCAL`, with a flag for whether the callee keeps the caller's
receiver, which only the caller's own script's handlers do). A call to a
name no handler in the corpus defines goes to the builtin first
(`LB_CALL_BUILTIN`, both forms), since no lookup could find anything;
what the builtin does not answer takes invoke's whole path, where the
file objects and the unimplemented alert live. The rest resolve by name as
before. An explicit receiver in the first argument still resolves by name
at run time, as classic method syntax requires.

Bound and builtin sites are most of the corpus: Mucklas 2,004 bound,
5,743 builtin, 1,371 by name; Deutsch 280, 8,478 and 419.

| Mucklas train | symbols as ids | calls bound | plus builtins first |
| --- | ---: | ---: | ---: |
| µs per Lingo step | 33.45 | 32.49 | 28.13 |
| window maximum work | 2.91 s | 2.89 s | 2.32 s |
| data misses (4 M floor) | 24 M | 24 M | 20 M |
| code misses | 46.7 M | 46.8 M | 41.1 M |

Parity on all six ports against the symbol-id probes: exact. The first
attempt bound a behavior's call to a movie-script handler with the
behavior's receiver, which the runtime never did; Christmas caught it in
one state, hence the flag.

### The frame advance reads only the film loops

With calls bound, the frame advance was the largest single consumer of
data misses (11%): every frame it walked every member-bearing channel to
find the film loops, comparing each sprite's member with the one it saw
last, and looked the member up again for each loop. The set of film-loop
channels is now kept current where every member change already reports
(`event_channel_changed`), with the member each holds, and the advance
walks that bitmap; the score pass clears only the change masks it set
instead of the whole table. Mucklas train: 28.18 to 26.1 µs per step,
window maximum work 2.73 to 2.43 s, and the frame advance is out of the
top of the profile. Parity on all six ports: exact.

### Three smaller reads

The caller attribution the profiler gained named three more. Every bound
name read three parallel tables (text, dispatch id, symbol id); one
record per name reads one line. The dynamic paths (`do`, `value`) looked
globals up by walking hundreds of names; a hash index answers them. And a
builtin applied to a receiver copied the receiver and its arguments into
a 776-byte array, when the method ops leave them contiguous on the
frame's stack already. Mucklas train: 26.1 to 24.81 µs per step. Parity
on all six ports: exact.

### One draw order per tick

The hit test and the renderer each asked for the draw order every tick,
and each walk read the type and depth of every active channel. The order
is now kept from one request to the next until a sprite change, a trail,
a channel leaving the active set or a tick boundary invalidates it, so
the renderer reuses what the hit test computed. Mucklas train: 24.81 to
23.93 µs per step. Parity on all six ports: exact. Hand-written tests
that write sprites directly report the change now, as the runtime's own
paths always did.

### Property slots and the site cache

The two A3 items left after the symbol work, measured on the train scene
of the tree that closed B3 (24.61 µs per step):

- **Property slots.** A declared script property is read and written at
  the pair its declaration order put it (`LB_SELF_SLOT`, `LB_SET_SELF_SLOT`:
  the compiler's hint is the pair index, the runtime checks it by one
  word compare against the pair's key and falls back to the named lookup).
  On the train it changed nothing measurable (24.78): the runtime's
  property memo already answered the hot reads, and behaviours without a
  `new` handler never materialise their declarations in order, so the
  hint misses on them. The ops stay, since they are exact and cost
  nothing, but the gain the roadmap expected is not there.
- **The site cache.** A call by name from a bytecode site is cached by
  the pooled name's address and the calling handler, so a hit is three
  word compares and no string compare, and the local-handler scan that
  every receiverless call ran first is part of the cached answer; an
  overlay coming or going retires every entry by generation. The table
  keeps the bytes the name-keyed set took, so nothing after it in the
  runtime moved in the data cache. Train: 24.78 to 24.03 µs per step.
- **Precomputed event and mouse-handler tables** were not built: the
  profile shows neither `declares_mouse` nor the event-mask computation
  in the top forty data-miss functions, because both are memoised once
  per script and the memos never miss on the tick path. A table would
  delete two memos and save nothing measurable.

Per port, against the tree that closed the three fixes:

| Port | scenario | before | after | ceiling |
| --- | --- | ---: | ---: | ---: |
| Mucklas | train | 24.61 | 24.03 | 53.5 |
| Christmas | lockstep | 52.50 | 51.28 | 88 |
| Willy | 10.DXR entry | 74.06 | 69.29 | 112 |
| Löwenzahn | lockstep | 110.87 | 111.30 | 129 |
| Deutsch | showcase | 134.08 | 126.27 | 205 |
| Workshop | boot | 233.16 | 221.86 | 294 |

What remains on the tick path by name is a cache miss: `lv_scan` compares
the folded text of a bucket's candidates, and the method resolution in
director.c keeps its four-way memo. A3's gate wording asked for no
string compare at all; the profile says the ones left cost under one
percent, and the roadmap records them rather than chasing them.

### A3 on every port

Each port's gate capture, built and run with the A3 compiler and runtime
(the first number is the last A2-era capture of the same scenario):

| Port | scenario | before | after A3 | ceiling |
| --- | --- | ---: | ---: | ---: |
| Mucklas | train | 50.29 (before A4) | 23.93 | 53.5 |
| Christmas | lockstep | 79.63 | 52.10 | 88 |
| Löwenzahn | lockstep | 152.83 | 106.37 | 129 |
| Deutsch | showcase | 203.25 | 128.1 | 205 |
| Willy | 10.DXR entry | 109.34 | 75.51 | 112 |
| Workshop | boot | 291.36 | 229.79 | 294 |

Willy's first A3 capture failed its object-growth gate at 1.71x against a
limit of 1.7: symbols left the object count, which took about a thousand
of them out of the baseline the ratio divides by, so the same scene
growth that measured 1.54x now measures 1.71x. The limit is 1.9 with that
reason recorded. Workshop's first run was invalid for another reason:
its ROM was built while the tree was being edited, which the build
receipts caught (`source/compiler/ABI inputs changed`), and it was rerun
on a quiet tree: all 35 journey scenarios pass, and the boot gate reads
229.79 against 294. Every port is inside its ceiling with more headroom
than it has had since the gates were set.

Next in A3: property slots for declared script properties, and the
cross-movie import tables for the calls that remain by name.

## A4 evidence: the runtime by family

A4's last clause asked for the runtime split by Director family, or for
converter-generated version-specific parts, in place of the preprocessor
guards and the "feature never widened" failure mode. The count on
2026-09-27 was 430 guards over the runtime and the console platform,
written as version arithmetic: `>= 7` in one place meant the keyboard,
in another the recorded draw blocks, in a third the in-place collector,
and widening any of them was a search through three files and a guess at
which sites meant the same thing. A physical split into per-family
sources would copy thousands of shared lines to remove them; the
evidence did not support that.

What landed instead is `runtime/lingo/family.h`: the six families the
ports are (D5, plain D6, extended D6, D7, D8, D10) and, derived from
them, the capabilities the guards actually mean, each defined once with
its reason. Every guard in the runtime, the console platform and the
game runtimes now names a family or a capability; the version number
appears nowhere else. The D7-and-later class that the failure mode came
from is split by feature: the keyboard, the sprite constraint, the wide
tables, the in-place collector, the sixteen-bit random bound, the
recorded draw blocks, the cache counters and the heap sweeps. The
conversion is a rename: parity is exact on all six ports and every native
test passes.

With the names in place, widening is one line each, and two were tried
on the D5 and D6 ports, which had never had them:

- **Recorded draw blocks** (`DG_CAP_DRAW_BLOCKS`): a cached image's RDP
  commands recorded once and replayed while its sprite's rectangle and
  opacity hold. Written for D7 in round 8, never widened. On every port
  now; the D5 and D6 captures' render time, summed over the capture:

  | Port | render before | render after | per step before | after |
  | --- | ---: | ---: | ---: | ---: |
  | Willy | 14,810 ms | 11,586 ms | 69.29 | 69.77 |
  | Löwenzahn | 16,294 ms | 8,928 ms | 111.30 | 109.06 |
  | Workshop boot | 227 ms | 235 ms | 221.86 | 220.12 |

  Löwenzahn's render time nearly halved and Willy's fell by a fifth, for
  35 to 43 KB of recorded commands; the per-step figures are Lingo time
  and do not move. This is the failure mode the roadmap named, measured:
  the feature had sat behind `>= 7` for two weeks with the two ports that
  gained most from it excluded.
- **Cache counters** (`DG_CAP_CACHE_STATS`): the `NATIVE_CACHE` line every
  D7 capture reported now comes from the D5 and D6 ports too, so their
  image traffic is measurable (Willy's 10.DXR entry: 217 decodes, 10
  evictions; Löwenzahn's lockstep: 84 and none).
- **The allocation chain and the in-place collector**
  (`DG_CAP_ALLOCATION_CHAIN`, `DG_CAP_EMERGENCY_COLLECT`): collection on
  allocation failure inside a step, which lets the runtime reserve 256
  handles instead of a quarter of the table. Tried on every family: Willy
  (extended D6, which had the chain but not the collector) collected 211
  to 177 times per capture for 7,983 to 6,924 ms of pauses, and its
  sanitized journey, which once exhausted the handles at the smaller
  reserve, passes with the collector behind it. D5 and plain D6 collect
  nothing in their captures, so the chain's bookkeeping bought them
  nothing and cost Workshop's boot 220 to 238 µs per step. Kept for
  extended D6, not for D5 and plain D6; the header says why in one line.

Every gate on the tree that closes the roadmap, µs per step: Mucklas
train 24.02, Christmas 51.28, Willy 72.44, Löwenzahn 109.05, Deutsch
126.31, Workshop boot 220.16; parity exact on all six ports; the
station's soak holds 0.996 delivery in both quarters with no skipped render.

The family header did not change what runs on any port by itself; what
it changed is the cost of the question "which port lacks this", from a
search to a line, and the first afternoon of asking it halved a render
time and removed a sixth of a port's collector pauses. The physical
split the roadmap offered as an alternative is not done, and the
evidence says it is not needed: the guards are named, counted and
justified where they are.

## A5 evidence: the evaluator

A5 asked for a restricted `do`/`value(` evaluator on device and
converter-time lowering of literal arguments. The evaluator on device is
the one the ports have run all along (`invoke_do`, `lv_literal` and the
data-assignment path in runtime/lingo/lingo_runtime.c), covered by the
fuzz baseline and every capture. The lowering has almost nothing to
lower: counted over every port's program on 2026-09-27, the `value(` and
`do` sites take a variable, a field's text or a chunk expression, and a
string literal at exactly two of them (one in Löwenzahn, one in Deutsch):

| Port | `value(`/`do` sites | literal-string arguments |
| --- | ---: | ---: |
| Workshop | 16 | 0 |
| Christmas | 9 | 0 |
| Mucklas | 49 | 0 |
| Willy | 48 | 0 |
| Löwenzahn | 5 | 1 |
| Deutsch | 134 | 1 |

A5 is therefore closed by measurement rather than by a feature: the
evaluator stays, its inputs are runtime strings, and a converter-time
path for two sites would be code without a workload.

## B1 evidence: source-depth textures

Every image was stored at sixteen bits per pixel whatever its source
depth, and a scene entry decompressed all of it. The packer now stores an
image whose plane holds at most 256 distinct RGBA5551 words as indices
over its palette (FDIC, `runtime/director/ink.h`): four bits per index
up to sixteen colours, eight beyond, tile-packed like every plane. A word
carries its alpha bit, so a colour that occurs both opaque and
transparent, a matte's white, is two palette entries, and the console's
per-ink rewrite runs once per colour instead of once per pixel. The RDP
looks each index up in the palette on its way to the framebuffer, so the
drawn pixels are the same: captured frames match the previous build's to
the codec's macroblock noise. The host probe and every test keep reading
the recovered sixteen-bit images; only the packed asset changes, behind a
per-port `image_indexed` policy.

Of the images the models reference, Workshop indexes 3,980 of 3,982,
Christmas 1,703 of 1,704, Willy all 1,580, Löwenzahn 1,411 of 1,414 and
Mucklas 330 of 499 (its watercolour art is the rest). Decompressed bytes
across each port's image set: Workshop 63.8 to 33.1 MiB, Christmas 67.3
to 32.9, Willy 78.6 to 39.3, Löwenzahn 116.9 to 59.3, Mucklas 56.9 to
40.0. Deutsch stayed direct-colour at first: its mask ink writes
per-pixel coverage that an index cannot carry, and the runtime skips
masking on an indexed texture rather than expand it. Since the mask is
baked at pack time (B2 evidence below) Deutsch indexes too, but only
1,097 of its 11,277 images have 256 colours or fewer: its art is
photographic, and its full-stage planes stay at sixteen bits.

| Port | gate | ROM before | ROM after | decompression per capture | outcome |
| --- | --- | ---: | ---: | --- | --- |
| Workshop | boot | 21.20 MiB | 18.50 MiB | 335 to 226 ms | passing; overruns 2 to 0, slow windows 2 to 1 |
| Christmas | lockstep | 19.19 MiB | 16.77 MiB | 278 to 190 ms | passing; overruns 9 to 2, min delivery 0.97 to 1.0 |
| Willy | 10.DXR entry | 24.16 MiB | 20.66 MiB | 705 to 498 ms | passing; min delivery 0.892 to 0.954, overruns 42 to 37 |
| Löwenzahn | lockstep | 54.95 MiB | 47.84 MiB | 443 to 299 ms | passing; min delivery 0.976 to 0.999, overruns 18 to 10 |
| Mucklas | train | 52.06 MiB | 50.52 MiB | 1016 to 741 ms | passing; 23.85 µs per step against 53.5 once the statics had a footing (below) |

The ROM bytes freed are what B3 spends: a smaller image set can sit at a
faster codec level under the 56 MiB ceiling.

### B1 evidence: the static footing

The first B1 capture of the Mucklas train scene read 27.07 µs per Lingo
step against 23.84 for the same scenario on the A3 runtime, and every
phase was slower from the first tick: render, audio and the Lingo service
alike, by 15 to 20%. The assets were not the cause. The A3 runtime over
the B1 packer's sixteen-bit images measured 23.83, and the B1 runtime over
the same images 27.80. Two runtime hypotheses failed as well: packing the
image record changed nothing (26.92), and compiling the interpreter's
dispatch chains without jump tables, on the guess that a table in the
read-only data had landed on the stack's cache sets, made it worse (27.69)
and raised code misses by a fifth. Both are reverted.

The emulator's miss profile, folded into the data cache's 8 KiB period
(`tools/cache-profile`, the slice table), showed the cause. 22.7% of all
data misses fell in one 256-byte slice of the period, shared between the
main stack's hottest frames, 0x900 below the top of RAM, and the cluster
of small statics around the .data/.bss boundary: the interrupt depth and
status the exception handler saves, the tick base every timer read adds,
the display's pending-frame counters and the runtime's cost counters. B1's
768 bytes of code had moved every static by that much, and this cluster
from a quiet part of the period (slices 9 to 14 of 32) onto the stack's
slices (19 to 24). The A4 placement of the two runtime structures, period-
aligned and offset a quarter and a half period in, had assumed the statics
near the period boundary, which was true of that build alone; the ±3% per
port that every A2 and A3 step recorded as placement noise was this same
mechanism at a smaller scale.

`platforms/n64/n64.ld` is libdragon's script with the read-only data, the
initialised data and the heap start each at a fixed footing in the period.
The data starts 0x1900 in, chosen by scoring every 256-byte footing
against the two profiles (per slice, the product of the statics' misses
and everyone else's), which keeps the cluster across the period boundary
where the profile has the fewest other hot lines. A link-time assertion
holds the cluster off the stack's slices, so a change to the statics that
crosses the margin fails the build naming the file to edit, instead of
costing 16% silently. The ROM reports the section starts in
`NATIVE_LAYOUT`, and the profile tool attributes misses to them. The cost
is up to 30 KiB of padding in RAM: Mucklas's minimum free memory on the
train scene went from 644,776 to 619,224 bytes.

| Mucklas train build | µs per step | data misses | code misses |
| --- | ---: | ---: | ---: |
| A3 runtime, B1 packer, sixteen-bit images | 23.83 | 18.9M | 42.8M |
| B1 runtime, indexed images | 27.07 | | |
| B1 runtime, packed image record | 26.92 | | |
| B1 runtime, no jump tables | 27.69 | | |
| B1 runtime, sixteen-bit images | 27.80 | 23.1M | 51.1M |
| B1 runtime, indexed images, static footing | 23.85 | 19.9M | 44.2M |

Miss totals are floors to the nearest million.

Every port was rebuilt on the footing and its gate rerun (the Makefile is
a build-receipt input, so each was a full rebuild). Per step, with the
port's last A3 capture and its first B1 capture for comparison; the memory
column is the padding's cost, the difference in the minimum free memory
the capture observed:

| Port | scenario | A3 | B1 | B1 with footing | ceiling | memory |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Mucklas | train | 23.93 | 27.80 | 23.85 | 53.5 | 25.0 KiB |
| Christmas | lockstep | 52.10 | 51.35 | 50.25 | 88 | 17.1 KiB |
| Willy | 10.DXR entry | 75.51 | 73.52 | 74.01 | 112 | 16.8 KiB |
| Löwenzahn | lockstep | 106.37 | 111.87 | 111.24 | 129 | 17.3 KiB |
| Deutsch | showcase | 128.10 | (not rebuilt) | 130.14 | 205 | 14.2 KiB |
| Workshop | boot | 229.79 | 235.14 | 235.21 | 294 | |

Every gate passes, and tick delivery, overruns and dropped time are the
same before and after the footing on every port; the footing's effect is
Mucklas's, where the statics had landed on the stack. Löwenzahn reads
five percent more per step under B1 than under A3, before and after the
footing, so that is not the statics: nearly every one of its draws now
uploads a palette, and the per-draw upload is the suspect to measure when
B2 reworks the draw path. Deutsch carries the B1 runtime with indexing
off, and its two percent is within the placement noise the A2 and A3
steps recorded.

## B2 evidence: scene bundles

Every image used to be its own compressed file in the ROM filesystem: a
scene entry opened one per sprite, decompressed it into the malloc arena
(or, on Mucklas alone, into a reserved region with its own compactor) and
evicted by least recent use once the cache filled, with no idea of what
the scene was about to draw. The host probe now records what the stage
draws: `director64 working-sets` runs the port's journey and 48 fuzz
episodes with `DIRECTOR64_DRAW_RECORD` set (platforms/native/draw_record.inc
counts every drawn bitmap and film-loop pose per movie, asset and ink,
exactly as the console's render pass asks for them), folds the counts with
the score's own references, and writes `host/working-sets.json`, a build
input. The packer (src/director64/image_pack.py) turns it into one
`images.pack` per port: every compressed image once, an index by authored
key for the fallback, and per scene the directory of its (asset, ink) pairs,
most drawn first. The per-image files leave the filesystem.

At a scene entry the console (platforms/n64/director_main.c) reads the
scene's directory and takes the block right after the drain, before the
scene's overlays, as the old Mucklas region was taken: the arena is
coalesced at that moment and nowhere else. The block is one size for the
whole session, the old cache's size (the 3,456 KiB region on Mucklas, the
3 MiB arena ceiling elsewhere) or what the first scene's overlays and
half a megabyte of headroom leave of the drained arena if that is less,
so the hole a block leaves is the hole the next one fills. A scene whose
whole directory fits gets every row pinned at the block's bottom, decoded
in place the first time the scene draws it, with the rest of the block
for the images no directory lists; a scene that draws more than the
block holds pins the rows the stage drew at least half the time (each
entry carries its share of the scene's most-drawn image) and leaves the
rest to the overflow cache. A scene with no directory takes no block and
keeps the arena cache it always had, which the boot movies need for the
shared casts they preload. D10 keeps no block at all: its scenes hold
megabytes of cast overlays across scenes and load more mid-scene, and the
arena cache with its headroom sweeps is the storage that fits around
them; the directories still tell its loader where every image is.

Four designs were measured on the way to this one. A purely static
bundle, the directory's first rows and nothing else, starved every scene
whose recorded set exceeds RAM: Mucklas's house went from no evictions to
112 in the capture, because a house's rooms are drawn one at a time and a
prefix by draw count pins the wrong ones, and the station lost the
compaction that keeps its pavement strips placeable. Pre-applied ink
variants, one blob per (asset, ink), were dropped because an image drawn
with both copy and matte inks needs two copies: Löwenzahn's pack grew from
24 to 43 MiB, past the ROM ceiling with its video. A block sized per scene
and taken before the overlays fragmented the arena scene by scene on
Deutsch, whose kept overlays fenced each smaller block's hole off from the
next larger one (largest free run 4.6 to 1.5 MB in six scenes); taken
after the overlays instead, it found only 2.3 MB of the station's 3.7 MB
free, because the D8 overlays and their link temporaries interleave, and
the house went to 371 evictions. One size, taken first, is what the
arena tolerates.

Per port, the gate capture on the pack against the last B1 capture. The
image traffic is the capture's total of decodes and evictions, and the
free memory the minimum the capture observed; the D5 and D6 ports report
no cache counters, so their rows show only what the pacing summary
carries.

| Port | scenario | per step B1 | per step B2 | loads B1 | loads B2 | evictions B1 | evictions B2 | min free B1 | min free B2 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Mucklas | train | 23.85 | 24.33 | 160 | 160 | 3 | 5 | 0.59 MiB | 0.77 MiB |
| Christmas | lockstep | 50.25 | 51.25 | 93 | 93 | 0 | 0 | 3.07 MiB | 1.47 MiB |
| Willy | 10.DXR entry | 74.01 | 68.82 | | | | | 1.26 MiB | 0.39 MiB |
| Löwenzahn | lockstep | 111.24 | 112.12 | | | | | 3.30 MiB | 1.32 MiB |
| Deutsch | showcase | 130.14 | 130.70 | 713 | 658 | 515 | 458 | 0.47 MiB | 0.50 MiB |
| Workshop | boot | 235.21 | 234.76 | | | | | | |

Every gate passes, and the station's hour-long soak holds 0.995 tick
delivery in its first and last quarters, with no skipped render and
708 KB free at the bottom (the last record on the hand-sized region was
0.976 to 0.975). The
ports whose scenes fit their block draw without a single eviction or
repack; Mucklas's house and station read exactly as they did with the
hand-sized region, now with 190 KB more free memory at the bottom. The
free memory the others show is lower by design: the block reserves what
the arena used to lend lazily, and the headroom is what the block leaves.
Deutsch is the port the block cannot help yet: its scenes' recorded sets
run to 5 to 12 MiB of decoded planes, its cast overlays need the arena
the block would take, and so it keeps the arena cache; the pack alone
saved it 55 decodes and 57 evictions. Its mask ink is now baked at pack
time: for every image the scenes draw with mask ink whose members all
carry the same `<name>_mask` member (68 of them; an image also drawn
unmasked keeps the console's pass), the packer cuts the mask into the
coverage bits (image_pack.apply_mask, the console's algorithm) and flags
the pack entry, and the console skips its per-pixel walk. The showcase's
mask time fell from 2,283 ms to 5 ms. Indexing its textures, which the
bake made possible, reaches only the tenth of its images that have 256
colours or fewer; the planes it decodes most are photographic, so its
decoded bytes and evictions are unchanged. What would shrink them is a
lower prescale, a per-port decision left open.

After the three fixes that followed (the symbol spelling, the markup
escape and the baked mask, all 2026-09-27) every gate was rerun on the
same tree: Christmas 52.50, Willy 74.06, Löwenzahn 110.87, Deutsch 134.08,
Workshop 233.16 µs per step, Mucklas 24.61 on the train and
its soak 0.994 to 0.996 delivery with no skipped render; parity is exact on all six ports with the
oracle comparing text case-folded, and Willy's host journey passes again.

One crash the B2 captures surfaced was older than B2: the on-screen
keyboard printed the typed name through libdragon's markup-parsing
printf, which reads `$xx` as a font change and asserts on anything else
after a dollar, and so did the D6 field text, the D5 alert, the notice
and the fatal screen. `platforms/n64/text_plain.h` doubles `$` and `^`
in every text that reaches those calls; field text already went through
the paragraph builder as literal spans.

## B3 evidence: the codec budget

The codec tiers choose a level by decompressed size and nothing else, and
the ROM ceiling was met by hand per port: Mucklas promoted every image to
level 2 once `image_color_bits` made room, the others promote at 64 KiB.
With the pack, every image has its own level and the working sets say
which images the scenes draw, so the choice is a knapsack: a port's
`image_pack_budget_mib` says what ROM the pack may take, the tiers set
the baseline, and `src/director64/image_budget.py` spends the rest on the
promotions that save the most decoding time per ROM byte, the images the
walks drew in the most scenes and that pack best at level 3 first, every
level-3 promotion before any level-2 one. The packer compresses the
promoted images a second time at their new level, cached by source and
level beside the baseline blobs, so a build from scratch and an
incremental one produce the same pack. Images no scene drew keep their
tier.

Every port's budget is its pack plus what the ROM ceiling left it, less
half a megabyte. With that the solver promoted every image the walks drew
to level 1, in two steps, and the ROMs stay under 56 MiB:

| Port | budget | pack B2 | pack B3 | promoted | ROM B3 | decompression per capture B2 | B3 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Workshop | 49 MiB | 11.87 MiB | 12.78 MiB | 696 | 19.25 MiB | 228 ms | 67 ms |
| Christmas | 49 MiB | 10.55 MiB | 12.89 MiB | 641 | 19.05 MiB | 1,094 ms | 478 ms |
| Willy | 50 MiB | 15.16 MiB | 15.97 MiB | 219 | 21.39 MiB | 2,513 ms | 950 ms |
| Löwenzahn | 32 MiB | 24.34 MiB | 27.97 MiB | 542 | 51.45 MiB | 1,509 ms | 557 ms |
| Mucklas | 44 MiB | 38.97 MiB | 42.87 MiB | 1,109 | 54.17 MiB | 2,201 ms | 1,347 ms |
| Deutsch | 30.5 MiB | 26.70 MiB | 28.33 MiB | 1,482 | 53.16 MiB | 7,104 ms | 2,992 ms |

The time a capture spends decompressing images fell by more than half on
every port, with the same loads and evictions and the same per-step cost
(the pacing gates read Lingo time, which images do not touch); Deutsch's
showcase, the port that decodes most, spends four seconds less. The
cost is ROM alone: 0.8 to 3.9 MiB per port, spent where the budget had
nothing else to buy. The knob is per port so a port that grows (Löwenzahn
with more video, Mucklas with more art) gives its images back first.

The ceiling itself is not the cartridge's: the SummerCart64 and the
emulator take 64 MiB, but libdragon's USB debug channel writes its
scratch area over cart bytes from 56 MiB up (`DEBUG_ADDRESS_SIZE`,
docs/roadmap.md Risks and the ROM ceiling note). A release build that
does not initialise the USB log, or a libdragon built with a smaller
scratch area, could raise every budget by 8 MiB; the probe builds that
the captures read would keep the ceiling.

## Risks

- **Semantic drift.** Mitigated by parity on the fuzzer's command streams
  before any speed claim; the fuzz seed baseline (4130993319) stays pinned.
- **Number representation.** Lingo integers and floats share `lv_t` today
  with an `id` flag; A4 must keep `integral_operand` semantics exact.
- **`value(` strings.** A restricted evaluator may not cover every authored
  string; unrecognized input alerts and continues, as the original does.
- **D10 RAM.** Deutsch's resident code is the tightest budget; A1 and A4
  are what buy headroom there.
- **Texture inks.** Matte, mask and dark inks compare pixel values today;
  B1 has to reproduce them on palette indices and is validated by the
  boot-gate image checks and captures.

## Status

| Milestone | State | Evidence |
| --- | --- | --- |
| A1 Bytecode contract | landed 2026-09-26 | see below |
| A2 Static operators and builtins | landed 2026-09-26 | operators and name ids, see below |
| C Rust compiler stage | landed 2026-09-26 | the only generator since the Python one was retired at the start of A3; was byte-identical to it on all six ports, 7x faster |
| A3 Static names | landed 2026-09-27 | symbols as ids, calls bound at convert time, film-loop channel set, name records, one draw order per tick (2026-09-26): 34.07 to 23.93 µs per step on the train, every port 21 to 37% faster per step; then property slots (no measurable gain, kept) and the site cache for calls by name (Willy 74.06 to 69.29, Workshop 233.16 to 221.86, Deutsch 134.08 to 126.27); event tables not built, the memos cost nothing measurable, see below |
| A5 Evaluator | closed by measurement 2026-09-27 | the on-device evaluator stays; converter-time lowering of literal arguments has two sites in the whole corpus, see below |
| A4 Value model and layout | landed 2026-09-26, families 2026-09-27 | five tiers (loop hygiene, hot header, placement, 8-byte values, handler index, stack diet, sprite record): 50.29 to 34.07 µs per step on the train; then the runtime by family: every version guard names a family or a capability in `runtime/lingo/family.h`, and the first widenings halved Löwenzahn's render time and cut Willy's collector pauses, see below |
| B1 Source-depth textures | landed 2026-09-27 | indexed images (FDIC) on five ports, ROMs 3 to 7 MiB smaller, bytes decompressed per entry halved, frames identical; found and fixed the statics' cache footing (`platforms/n64/n64.ld`), which a 768-byte code change had cost 16% per step; every gate passes, see below |
| B2 Scene bundles | landed 2026-09-27 | recorded working sets, one image pack per port with per-scene directories, the scene block with pinned rows over the compacting region on every port but D10; the station's soak flat at 0.994 with zero skipped renders; see below |
| B3 Budget solver | landed 2026-09-27 | the codec budget: per-port pack budget, promotions by decoding time saved per ROM byte; decompression per capture down by more than half on every port under the 56 MiB ceiling; see below |

### A1 evidence

Parity: `director64 parity` on all six ports, four episodes of 120 actions
each from the fuzz baseline seed, 6,257 states compared across 45 movies,
zero divergences; the sanitized side reported no memory errors. A deliberate
off-by-one in the VM's small-literal op was caught on the first command.

Size, generated per port (C source excludes listings):

| Port | Generated C before | after | Bytecode |
| --- | ---: | ---: | ---: |
| Löwenzahn | 769,061 | 480,518 | 45,408 |
| Christmas | 1,842,239 | 914,327 | 192,689 |
| Willy | 2,563,421 | 1,498,979 | 199,378 |
| Workshop | 2,976,259 | 1,610,361 | 265,901 |
| Mucklas | 5,463,705 | 2,830,916 | 541,743 |
| Deutsch | 10,301,175 | 5,922,434 | 784,150 |

On the console, Christmas's three largest overlays went from 84,608 / 81,364
/ 67,968 bytes to 56,607 / 39,551 / 42,922, its ROM from 20,529,152 to
20,103,168 bytes; Workshop's ROM from 22,822,912 to 22,233,088.

Per-step cost, emulator captures (Christmas is lockstep at 1,769,528 steps;
Willy differs by 350 steps of 11.9 million):

| Port | µs per step before | after | ceiling | dropped before | after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Christmas | 83.42 | 83.08 | 88 | 233 ms | 200 ms |
| Willy | 106.62 | 110.27 | 112 | 6.09 s | 5.80 s |
| Löwenzahn (lockstep) | 151.44 | 152.83 | 129 boot | 624 ms | 559 ms |
| Mucklas train | 50.87 | 51.02 | 53.5 | | |
| Deutsch showcase (not lockstep, +5% steps delivered) | 216.66 | 203.25 | 205 boot | 14.20 s | 11.08 s |
| Workshop boot (4 windows) | 280.0 | 291.36 | 294 | 0 | 0 |

Every gate passes. The D5, D7, D8 and D10 profiles are neutral or better;
the two D6 profiles pay 3 to 4 percent more per step, which A2's dispatch
restructuring should absorb and must re-measure.

The first VM, a switch that called each `lx_*` service, measured 91.0 µs on
Christmas and failed the ceiling; inlining the op bodies into the loop is
what brought it back. A VM change is only judged on the console: host
parity sees semantics, not cost.

### A2 evidence: operators

Operators resolve to ids at build time (`lb_binary_t`, `lb_unary_t`); the
VM takes two numbers under an arithmetic or relational operator straight to
the numeric tail and everything else through the same chain as before, now
on integer compares. Parity: six ports, zero divergences. Lockstep captures:

| Port | µs per step before | after | ceiling |
| --- | ---: | ---: | ---: |
| Mucklas train | 51.02 | 49.43 | 53.5 |
| Willy | 110.27 | 108.70 | 112 |
| Christmas | 83.08 | 85.62 | 88 |

Christmas moved the wrong way with identical step counts and slightly less
dropped time; code placement on a direct-mapped cache swings a port by a few
percent either way, which the runtime-layout milestone (A4) is for. The
per-step floor is name dispatch, the next slice.

### A2 evidence: name ids

Every property, builtin and reference-kind name the runtime dispatches on
is numbered once in `runtime/lingo/names.txt` (414 names; the generated
`lingo_names.h` and `lingo_names.inc` are checked in and tested against
the sources). The converter emits the id of every pooled name beside the
name, a generated handler binds the id of each name it passes, and the
dispatch chains in director.c, the version files and the Lingo runtime
compare integers (678 literal compares converted). GCC 16 on the target
turns runs of those compares into jump tables at -O2 and partly at -Os.
Parity: six ports, zero divergences.

| Port | µs per step before | after | ceiling | steps |
| --- | ---: | ---: | ---: | --- |
| Christmas | 85.62 | 79.64 | 88 | lockstep |
| Mucklas train | 49.43 | 50.86 | 53.5 | −0.7% |
| Willy | 108.70 | 109.34 | 112 | +5.4% delivered, dropped 5.80 → 5.65 s |

Holding the binding in the frame instead of the runtime struct was tried
and measured worse on every port (Mucklas 53.23, Willy over its ceiling),
so the binding stays at the end of the runtime struct. On the host the
Director property chain is now under one percent of station time; what
remains there is the VM loop (31%), handler invocation (about 15%) and
behavior property access by name (about 12%), which is A3.
