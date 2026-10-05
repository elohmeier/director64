#ifndef DIRECTOR64_FAMILY_H
#define DIRECTOR64_FAMILY_H
// The Director families the runtime is built for, and the capabilities each
// one has. Every version-dependent piece of the runtime and the console
// platform is guarded by one of the names below, never by the version
// number: a capability is a fact about what a family's ports need, stated
// once here with its reason, and widening one to another family is one line
// in this file, where before it was a search for every `>= 7` in three
// files and a guess at which of them meant the same thing. The families are
// what the ports are: D5 (Löwenzahn), plain D6 (Workshop), extended D6
// (Willy, a D6 title with D7-era behaviours), D7 (Christmas), D8 (Mucklas)
// and D10 (Deutsch).
// Without a version the build is the plain D6 profile, as director.h has
// always read it: the host tools and the native tests compile that way.
#ifndef DIRECTOR64_DIRECTOR_VERSION
#define DIRECTOR64_DIRECTOR_VERSION 6
#endif
#ifndef DIRECTOR64_EXTENDED_D6
#define DIRECTOR64_EXTENDED_D6 0
#endif
#define DG_FAMILY_D5 (DIRECTOR64_DIRECTOR_VERSION == 5)
#define DG_FAMILY_D6 (DIRECTOR64_DIRECTOR_VERSION == 6 && !DIRECTOR64_EXTENDED_D6)
#define DG_FAMILY_D6X (DIRECTOR64_DIRECTOR_VERSION == 6 && DIRECTOR64_EXTENDED_D6)
#define DG_FAMILY_D7 (DIRECTOR64_DIRECTOR_VERSION == 7)
#define DG_FAMILY_D8 (DIRECTOR64_DIRECTOR_VERSION == 8)
#define DG_FAMILY_D10 (DIRECTOR64_DIRECTOR_VERSION >= 10)

// ---- Families as guards ----
#define DG_D5 DG_FAMILY_D5
#define DG_D8 DG_FAMILY_D8
#define DG_D10 DG_FAMILY_D10
// Willy's D6 runs behaviours, film loops, styled text and the D7 event
// model; its source-policy declares it extended.
#define DG_EXTENDED DIRECTOR64_EXTENDED_D6
// Director 7 and later: the engine model with behaviours as instances.
#define DG_D7_UP (DIRECTOR64_DIRECTOR_VERSION >= 7)
// The D7 engine's features that D5 and extended D6 share: everything but
// the plain D6 profile, whose score is the whole of its behaviour.
#define DG_MODERN (!DG_FAMILY_D6)
// The hover cache and member-number arithmetic of D7 and D10, which D8's
// recovered scripts contradict.
#define DG_D7_OR_D10 (DG_FAMILY_D7 || DG_FAMILY_D10)

// ---- Capabilities of the D7-and-later engine, each with its reason ----
// Keyboard events: dg_key, the key state, keyDown/keyUp dispatch. Extended
// D6 answers its keyboard through its own input driver (willy_input.c) and
// the controller keyboard; plain D6 and D5 scripts declare no key handler
// (counted 2026-09-27: Workshop 0, Löwenzahn 1 unused).
#define DG_CAP_KEYBOARD DG_D7_UP
// The controller keyboard for edit-text fields.
#define DG_CAP_TEXT_INPUT (DG_EXTENDED || DG_D7_UP)
// `the constraint of sprite`: no D5 or D6 port's scripts use it.
#define DG_CAP_CONSTRAINTS DG_D7_UP
// The wide tables: 801 sprite channels, 8192 objects, 512 event-miss
// records, the memo spread over the larger id space.
#define DG_CAP_WIDE DG_D7_UP
// Objects kept in an allocation chain so a freed hole can be reused and
// compaction stays linear; extended D6's car screens needed it for their
// collector pauses (2026-09-16). D5 and plain D6 collect nothing in their
// captures, and the trial of 2026-09-27 that gave them the chain and the
// in-place collector cost them per-step time for no pass saved.
#define DG_CAP_ALLOCATION_CHAIN (DG_D7_UP || DG_EXTENDED)
// The in-place collector that runs on allocation failure inside a step,
// which lets the runtime reserve 256 handles instead of a quarter of them.
// Widened to extended D6 on 2026-09-27: Willy's capture collects 16% fewer
// times and its sanitized journey, which once exhausted the handles at the
// smaller reserve, passes with the collector behind it.
#define DG_CAP_EMERGENCY_COLLECT (DG_D7_UP || DG_EXTENDED)
// `random()` clamps its bound to sixteen bits as ScummVM's b_random does.
#define DG_CAP_RANDOM16 DG_D7_UP
// Recorded RDP command blocks per cached image, replayed while the image
// and its sprite geometry stay the same. Written for D7; widened to every
// family on 2026-09-27 (trial: measured per port below).
#define DG_CAP_DRAW_BLOCKS 1
// The image cache's per-window counters (NATIVE_CACHE): instrumentation
// every port's captures can use.
#define DG_CAP_CACHE_STATS 1
// Heap sweeps that release completed images before a font atlas or an
// overlay needs one contiguous block.
#define DG_CAP_HEAP_SWEEPS DG_D7_UP
#endif
