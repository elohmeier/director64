# Roadmap: a converter-first Director64

Director64's product is the Android TV player: one APK per game, built from
the user's own disc image ([android.md](android.md)). The standalone
per-game web app ([web.md](web.md)) is a by-product of the same runtime and
conversion. This document records the architecture direction both share,
what has landed, and what is still open.

## Direction

The device never parses Director files or Lingo, and it should not bind
meaning by name either. The converter does the semantic binding and emits
data that the runtime runs without interpreting names:

- **Bytecode plus tables, not C.** Handlers compile to compact bytecode with
  resolved operands (`runtime/lingo/lingo_bytecode.h`). The runtime language
  and toolchain can change without touching the converter's output, and the
  same bytes run on Android, in the browser and in the Node probe.
- **Static resolution.** Operators, `the` properties, builtins and reference
  kinds are numbered once in `runtime/lingo/names.txt`. Symbols get
  corpus-wide ids. Calls the converter can answer are bound at conversion
  time. Only calls whose target depends on runtime state resolve by name.
- **A game package, not generated code.** Everything a converted game needs
  apart from the runtime itself sits in one validated, versioned package
  (`game.d64p`, [package-format.md](package-format.md)) plus its asset packs.
  It binds to the runtime through an ABI digest over `names.txt` and
  `lingo_bytecode.h`.
- **Runtime named by Director family.** Every version guard names a family
  or a capability defined once in `runtime/lingo/family.h` (D5, plain D6,
  extended D6, D7, D8, D10, with Director 8.5 as D10 plus
  `DIRECTOR64_D85`). To widen a feature to another family, change one line.
- **A small evaluator stays on the device** for `do` and `value(`. Their
  arguments are runtime strings at every site but two in the whole corpus, so
  lowering them at conversion time would add code with nothing to run on.

What stays in the runtime is the engine: score playback, sprite and event
semantics, ink compositing, audio, video and a compacting collector.

## Decisions

1. **Emit bytecode and tables, not C.** The converter's output is a data
   contract. The Android and web runtimes load it; neither links generated
   code.
2. **Parity before speed.** A change to the runtime or the converter must
   first reproduce the previous build's probe state stream under the fuzzer's
   command streams (`director64 parity`, [fuzzing.md](fuzzing.md)). Any speed
   gain is claimed only after that. The fuzz seed baseline (4130993319) stays
   pinned.
3. **Rust on the host, C on the device.** All converter stages after the
   ProjectorRays parse are Rust under `compiler/`, built natively for
   recovery and to WebAssembly for the conversion
   (`compiler/wasm`). The runtime stays portable C that compiles unchanged
   for Android (NDK) and the browser (Emscripten).
4. **Keep ProjectorRays as the parser.** It connects to the converter
   through the JSON dumps that already exist.
5. **Disc images stay local.** Each port pins one edition by SHA-256 (plus
   alternate rips of the same pressing). Conversion happens on the user's
   machine, and an APK or web build that contains converted data is never
   redistributed.

## What has landed

The first converter-first round, mostly in September 2026, is complete:

| Milestone | Result |
| --- | --- |
| Bytecode contract | Handlers run as bytecode in `lv_vm_step` over the `lx_*` services. Step accounting and collection points are unchanged, and parity was exact on every port. |
| Static operators and builtins | Operator ids (`lb_binary_t`, `lb_unary_t`) and 414 numbered names. The runtime dispatch chains compare integers, not strings. |
| Rust compiler stage | `director64-aot` in `compiler/` is the only bytecode generator. It replaced the Python generator after reproducing its output byte for byte. |
| Static names | Symbols are ids; calls are bound at conversion time (`LB_INVOKE_LOCAL`, `LB_CALL_BUILTIN`); handler tables are indexed by hash; declared properties use slot hints; calls that stay by name hit a site cache. |
| Value model | `lv_t` is one 64-bit word: boxed handles and integers, floats offset by 2^49, all-zero is VOID. Instances print by allocation serial, so parity does not depend on when collections run. |
| Runtime by family | `runtime/lingo/family.h`. Every guard names a family or a capability. |
| Evaluator | Closed by measurement: the evaluator stays, and nothing is lowered at conversion time. |
| Game package | D64P ([package-format.md](package-format.md)). The web and Android runtimes load it and validate every record before building anything. |
| Rust converter | Every stage after the ProjectorRays parse is Rust, including PFR1 fonts through a clean-room decoder. |

Two parity rules learned on the way still hold. The oracle compares symbols
case-folded, because a symbol's identity ignores case and its spelling is
whichever spelling the first script used. It also renumbers instances by first
appearance in the state stream.

## Open work

### Native conversion driver

Today a conversion runs under Node. `platforms/android/export/export.mjs`
drives `platforms/web/import/pipeline.mjs`, which loads the Rust converter
compiled to WebAssembly alongside the ProjectorRays parser, the font tools,
libwebp and FFmpeg. That driver is left over from the in-browser importer.
Now that conversion only runs on the command line, the plan is a native Rust
driver: the converter running as a host binary that calls the same stages
directly. The goals are one fewer runtime (Node and the wasm tool builds), a
simpler cache identity (`build/web/tools/manifest.json` and the import source
digest), and faster conversions. The gate is a byte-identical `game.d64p` and
identical asset packs for every port.

### Per-game converter code into data

`compiler/src/ports.rs` carries what each port adds beyond the generic
pipeline: launcher handlers from the embedded projector, compatibility fixes
to the recovered program pinned to the exact source, and model corrections.
Each fix is reviewed against one edition and refuses any other, which is the
right contract. The code that applies the fixes, however, is partly written
per game. The plan is to keep moving these fixes into data: the game's
`host/source-policy.json` and `game.toml`, with a generic interpreter in the
converter. That way a new port is mostly a policy file plus a reviewed list of
fixes. Capability rejections (`compiler/src/capabilities.rs`) stay as they are.
They protect builds while a feature is missing, and they do not count as
implementing it.

### Constraints left over from the N64 runtime

The runtime was first written for a console with 8 MiB of RAM and a
direct-mapped data cache. Some of that shape remains and no longer fits a
hosted target:

- **Fixed memory pools.** The Lingo heap (`LV_HEAP_BYTES`), object table
  (`LV_OBJECTS`), roots and shared tables are fixed per family at compile
  time (`runtime/lingo/lingo_runtime.h`), and D8.5 already needs 4 MiB and
  32,768 objects. Pools that grow on demand on hosted builds would remove a
  class of overrun alerts.
- **Cache-tuned struct layout.** Field order and padding in `lv_runtime_t`
  (the per-step scalars first, `LV_CALL_BYTES` sized to the table it
  replaced, appended rather than grouped tables) were chosen against a
  console cache profile. On ARM and in the browser that ordering only holds
  layout steady and can be simplified.
- **The 640x480 prescale path.** `runtime/director/ink.h` and the converter
  still carry the rule that prescales an authored 800x600 stage to 640x480.
  Android and the web build run with `native_stage` (`DIRECTOR64_NATIVE_STAGE`)
  and draw the authored stage unscaled, so the prescale branch and the
  policy's `stage_prescale` can go once nothing depends on them.

Each simplification is a parity change: identical probe state streams on
every port before and after.

### Recorded open items

- **Implementation gaps.** Features the runtime does not implement yet stay
  capability failures with diagnostics until they are implemented. They are
  listed in [implementation-audit.md](implementation-audit.md).
- **Catalogued games.** Findus' garden, Fritz & Fertig and Löwenzahn 2 to 5
  are catalogued but not ported ([games.md](games.md)).
- **Scene entry cost.** Steady scenes hold 60 ticks per second. What remains
  of measured drops is scene entry, mostly image loading
  ([performance-audit.md](performance-audit.md)).
- **Controller mappings.** Pads other than the standard Android gamepad
  layout (for example the ModRetro M64 Pro's C buttons) are unverified. The
  player's diagnostics overlay shows the raw keys and axes a mapping needs
  ([android.md](android.md)).

## Risks

- **Semantic drift.** Parity on the fuzzer's command streams comes before
  any change is accepted. The journeys and the seed baseline are the
  correctness oracle.
- **Number representation.** Integers and floats share `lv_t`; any value-model
  change has to keep `integral_operand` semantics exact.
- **`value(` strings.** The restricted evaluator may not cover every
  authored string. Unrecognised input alerts and continues, as the original
  does.
- **Memory on the TV.** The target projector has 2 GiB of RAM shared with
  the system. The Android build caps decoded images at 96 MiB, and pools
  that grow must stay inside a budget of that kind.
