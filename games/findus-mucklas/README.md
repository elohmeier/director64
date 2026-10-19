# Findus bei den Mucklas

Experimental Director 8 port of the locally supplied German disc. Converted
game data and saves stay in ignored game/source workspaces; the repository
contains the converter, runtime, adapters and tests.

```sh
uv run --locked director64 android build --game findus-mucklas
uv run --locked director64 android install --game findus-mucklas --serial <device>
uv run --locked director64 web --game findus-mucklas --serve
uv run --locked director64 journey --game findus-mucklas --activity
uv run --locked director64 journey --game findus-mucklas full_regressions
```

The manifest pins ISO SHA-256
`ad4c7afb4b19339c45c2a69756b522b59ba4869199740fd528527f190a806925`.
The converter processes 48 media archives and the embedded projector as the
`START.DXR` unit, and recovers all 630 scripts. `aot/program.json` retains the
media AST and records selected-port corrections. Source recovery retains the
original 48-byte score records, field validity masks and extended values. The D8
profile supports 800 sprites, twelve cast libraries, eight sound channels, RGB
score colors, rotation/skew, and the recovered tempo operands through 999 FPS.

## Assets

All 5,344 bitmap references, 1,285 sound references, and 21 film loops are
accounted for, including one authored empty bitmap and one empty film. The static
vector-shape Xtra is rasterized from its embedded SWF. All 61 text members retain
their source payloads and Paige typography. Both embedded font members contain
the same original Pettson PFR1 font: its 223 glyphs are recovered into OpenType
with cubic outlines intact, then rasterized at the seven authored sizes by the
pinned mkfont. The glyph-program decoder carries the curve-command corrections
recovered from the original Bitstream player (see
[tools/fonts/README.md](../../tools/fonts/README.md)); 128 Pettson glyphs pick
up small authoritative outline refinements from them, verified byte-identical
against the emulated original for every glyph. Player-name and score fields use
this font with authored colors, alignment, baselines and line spacing. Source
hinting is not exported; no original-projector pixel comparison is claimed.
Missing system fonts and unsupported authoring-field layout remain explicit in
`director/model.json`; `analysis/font-reachability-audit.json` distinguishes
concrete references from absence of reference evidence.

Soft-alpha images keep an exact A8 coverage plane beside their colour;
binary-alpha images use the smaller format. Film-loop composition also preserves
partial coverage. Images wider or taller than 1024 pixels (the train activity's
1182×127 `ts_st4`, its other wide backgrounds and BR's 1920-pixel images) are
stored as 32×32 tiles. Nothing in the file records the layout: the converter
(`plane_pixels` in `compiler/src/convert/fdi.rs`) and the runtime
(`runtime/director/bitmap_tiles.h`) derive it from the stored dimensions, and
`compiler/tests/image_tiles.rs` checks they agree, because a disagreement would
render every wide image scrambled rather than fail.

## Controls and saves

The stick (or a mouse) moves the pointer and A operates the original mouse
interface. Click the player-name field to type a name: the keyboard opens by
itself (the TV keyboard on Android, the on-screen keyboard for a controller in
the browser), every typed or deleted character reaches LO's original key
handlers at once, including letter sounds and the 20-character limit, and the
original confirmation button saves the name (`d64_key_field`). In KR and KB the
C buttons (a standard pad's right stick) supply arrow keys. Controller 2
supplies directions through its stick/D-pad and space through A; KR uses the
live localized second-driver bindings. Steering reads those two pads directly,
because a race belongs to the players at pads one and two rather than to
whoever holds the pointer (`runtime/controls.c`). Unplugging a pad or leaving
the activity releases its keys. Outside the races every pad points with its own
coloured cursor ([shared pointer](../../docs/architecture.md#players-and-cursors)).

The game adapter exposes only its fixed installation/save filenames and uses a
separate `M64D` journal in the save image. A fresh game starts without imported
player progress. Two independent 64 KiB generations preserve the previous commit
on a failed or torn write. A fresh installation also sets `Vind=0`, honoring the
source's gate for the VI attic activity absent from this disc.

## Behavior and corrections

ScummVM was clean at `41ac2b31847622d0662d22c03fe6979e3b43cfbc` on 2026-09-06.
The local implementation reference and recovered source are corroborated with
focused compiler and runtime tests.

D8 sprite constraints clamp the registration point against the live bounds of
the referenced channel during position changes and dragging. `keyPressed()`
queries held Macintosh key codes or characters; no held keys correctly returns
false during the invention mouse-up checks. Zero/truncated-zero random bounds use
the reference implementation's 1–65535 range, fixing FA's
`random(abs(distance - 40))` path while retaining the existing seeded generator.

Sprite width and height report the sprite's dimensions before transformation;
rect/edges describe the transformed bounds. The train station's piston script
feeds `sprite.height` into its next quad, and answering the deformed bounding
box made it grow exponentially. The ScummVM reference reads width/height from
sprite dimensions too; it does not verify D8 custom-quad behavior against the
original projector.

BR's two authored `getAt(avstList, a)` calls use symbol keys with a property list.
Their bytecode extcall offsets are 546 and 579 in member 4's `ritaBana` handler.
`compiler/src/ports.rs` corrects those calls to `getaProp` for the pinned disc and
script hash, and fails if either source identity or the two expressions change.
Generic `getAt` still requires a numeric index; the original recovered AST stays
unchanged. The program records this correction explicitly. It is not verified
against the original projector.

Symbols are interned as Director interns them: a literal evaluated in a loop
yields the object it yielded last time instead of a fresh copy. Handler calls
clear declared locals; flat list duplication retains the same object identity
semantics. Method caches are cleared when movies change, including cached misses.

## Evidence and limits

The activity journey (`--activity`) creates a fresh player, enters the house,
restarts, reloads that player, and solves the clock activity through the
original controls. It checks all three correct arrivals, the reward animation,
the inventory addition, and persistence across another restart. The regression
journey (`full_regressions`) replays the audited activity entrances and formerly
failing interactions from a fresh installation, including invention
release/drag, NB's fly, BR's course drawing, FA's catching phase, the train
station's passenger-count question and its piston animation, and typing,
deleting, saving and reloading a player name through the recovered LO scripts.
This extends entrance and interaction coverage, not completion coverage for
every activity.

Remaining work includes full activity completions, second-player playthroughs,
double-click behavior, unresolved font/layout approximations, and an
original-projector comparison.
