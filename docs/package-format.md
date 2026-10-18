# Game package format (D64P, version 9)

A game package carries everything the runtime needs to run a converted game
that is not the runtime itself: every movie's handler bytecode and name
pools, the corpus symbol table, the global slots, and each movie's scene
tables (casts, members, score, labels, palette). The browser runtime loads
one instead of linking the generated C (docs/web-roadmap.md, W2). The console
keeps its generated C, the transition path, until the loader has the same
evidence there.

`compiler/src/package.rs` writes it. `runtime/package/package.c` validates
and loads it into the structures `runtime/lingo/lingo_runtime.h` and
`runtime/director/director.h` define. Nothing in it is a serialized C
structure or a pointer. Every integer has a stated width and is
little-endian, and every reference is an offset or index that the loader
bounds-checks before building anything.

## Header

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `D64P` |
| 4 | 2 | Format version, 9 |
| 6 | 2 | Director version the tables are laid out for: the port's (500, 600, 700, 800, 1000; a Director 8.5 port is laid out as 1000) |
| 8 | 2 | Profile flags; bit 0 is extended D6 |
| 10 | 2 | Section count |
| 12 | 16 | ABI digest: the first 16 bytes of SHA-256 over `runtime/lingo/names.txt` followed by `runtime/lingo/lingo_bytecode.h` |
| 28 | 4 | Reserved, zero |
| 32 | 12 × n | Section table: tag (four ASCII bytes), offset, length (u32 each) |

A runtime is built for one family (`runtime/lingo/family.h`) and refuses a
package laid out for another, and one whose ABI digest differs from its own. Name ids
and opcodes are positions in those two files, so any change to either
changes what the bytecode means.

Sections may appear in any order. Each tag appears exactly once, and
sections neither overlap the header nor extend past the file.

## Sections

`str` is a u32 offset into `STRS`. `STRS` must end with a NUL byte, so every
offset below its length names a NUL-terminated string.

| Tag | Contents |
| --- | --- |
| `META` | UTF-8 JSON: game slug, source identity, converter revision, profile. Informational; the runtime does not read it |
| `STRS` | String pool |
| `CODE` | Handler bytecode, concatenated |
| `SYMB` | u32 count, u32 bucket count (a power of two), count × `str`, (bucket count + 1) × u32 bucket starts |
| `GLOB` | u32 count, count × `str`: global names in slot order |
| `MOVI` | u32 movie count, then that many movie records in registry order (movie ids 1..n) |

A movie record:

```
str   code name                       ("VEMORY.DXR")
u32   handler count, then per handler:
        str name; u32 member; str cast; str kind;
        u32 arguments; u32 local count; u32 entry; u32 property count;
        local count × str; property count × str;
        u32 code offset; u32 code size        (within CODE)
u32   name count, then per name: str text; u16 name id; u16 symbol id
u32   double count, then that many f64
u32   handler bucket count; when there are handlers:
        handler count × u16 order; (bucket count + 1) × u16 bucket starts
u16   movie id; u16 tempo
modern:   u32 stage color; u16 style count, then per style:
            str font name; u8 font id, size, align;
            i16 ascent, descent, leading, line height; u32 color;
            extended: u16 advance count (0xFFFF: no measured metrics), then
              that many u8 advances from codepoint 32, u32 kerning pair
              count, u32 value count, value count × i16 (first, second,
              amount per pair)
u16   cast count, then per cast: str name; u16 file; u16 cast
        modern: u8 pattern catalog present (0/1); when present, eight str
          texture assets for patterns 57..64 (empty string: absent slot)
u32   member count, then per member:
        u32 id; u16 number, cast, type, width, height; i16 reg x, reg y;
        str name, asset, text;
        u32 samples, rate, loop start, loop end;
        u16 shape, pattern; u8 filled, line width, looping; u16 film
        count; u8 film loop, line direction; D10: u8 Flash phase variant count (1..255);
        film count × variant count × str film assets in frame-major order
        (other profiles have one variant per frame)
        extended: u8 editable; u32 binary text length, and when nonzero
          that many bytes plus a NUL (the member text, NULs included);
          u32 source bytes; u8 cue count, then per cue: u32 ms; str name
        modern:   u16 text style, u16 insert style (1-based, 0: none)
        D5:       str video audio; u32 video flags; u32 film sound count
                  (0, or two per film frame), then that many u32 references
        D10:      u8 source Xtra (0 native, 1 Flash, 2 vectorShape, 3
                  DirectMedia video); str video audio; u32 video flags;
                  u32 film sound count; u32 member ids[film sound count];
                  u8 Flash label count; u8 Flash field count;
                  labels (str name; u16 frame); fields (str name, variable,
                  text; i16 x, y, width, height, left margin, right margin,
                  indent; u8 align, word wrap, multiline; i8 leading;
                  u16 style; i32 local bounds[4] in 16.16 pixels, ordered
                  left, top, width, height); u8 embedded fonts (UseOutlines);
                  u16 text colour run count, then per run:
                  u32 first character, u32 0xRRGGBB; u16 hyperlink count,
                  then per link: u32 first, last character (1-based,
                  inclusive); str data; u16 linked movie registry id
                  (0 for other cast types); u32 linked movie flags;
                  i16 linked stage left, top; u16 runtime film frame count;
                  i16 film source left, top; each runtime film frame has
                  u16 layer count followed by channel-ordered layers, each
                  u16 sprite channel, u16 zero-based nested film pose,
                  and the complete score spec below
                  (no script/behavior dispatch in film subchannels)
                  then str direct raster asset, str Flash object/field name,
                  str geometric mask asset (opaque fills without strokes),
                  i32 authored object origin x/y in 16.16 stage pixels;
                  direct planes have member id 0 and preserve depth order
                  then u8 affine flag and six i32 16.16 matrix components
                  [a,b,c,d,tx,ty] for a live text depth plane
                  followed by u16 Flash stop count and u16 authored one-based
                  stop frames; these stop on entry, advance and explicit seek
                  then u16 translated Flash helper count; each helper is
                  str name, target object, property; u8 operation (1 numeric
                  add, 2 set, 3 whole-field font size, 4 unescaped text),
                  u8 argument index (zero-based without receiver), u8 method
                  (1 if receiver is the target field, 0 for its containing sprite)
      member count × (u32 name hash; u16 member position)
256 × u32 palette
u32   frame count, then per frame: u32 first delta; u16 delta count
u32   delta count, then per delta:
        u16 channel; u16 mask;
        u32 member, script; i16 x, y, width, height;
        u8 ink, blend, type, flags, fore, back, thickness, stretch, trails
        modern:   i16 z; u16 tempo, delay; u32 fore RGB, back RGB;
                  i32 rotation, skew
        extended: u8 behavior count, then per behavior: u32 script;
                  str parameters
u16   label count, then per label: str name; u16 frame
```

"modern" is every family but plain Director 6 (`DG_MODERN`); "extended"
is the extended-D6 service set, which D7 and D10 ports use as well.

The loader checks every count against what remains of the section, every
string against the pool, every code range against `CODE`, every handler
order, bucket, frame and member index against its table, and that movie ids
count 1..n in record order. A failed check rejects the whole package, and
no partially built movie is ever reachable.

Flash members additionally store u32 intrinsic frame rate in 16.16 format,
then u16 stream position count and that many u32 PCM sample offsets. A nonzero
count equals the logical film frame count; 0xFFFFFFFF denotes a frame without
a stream block. The existing video-audio asset, samples and rate fields carry
the decoded stream. Phase variants multiply raster and live-layer storage,
while labels, stream positions and the root playhead retain logical frames.

The Flash stream fields are followed by u8 playback mode (0 Normal, 1 Fixed,
2 Lock-Step), u32 fixed frame rate in 16.16 format, u8 direct-to-stage, and
u8 original SWF version. The latter controls AVM1 version-dependent behavior.
The member's existing film loop and video paused-at-start flags preserve the
verified Flash Xtra descriptor values.

Retained interactive Flash definitions follow these fields. Four u16 counts
precede the clip, button, sound and export tables. Character IDs share one
namespace with field definitions; ID 0 denotes the root clip. Each clip stores
u16 ID and frame count, four signed 16.16 local bounds, its frame layers
(the root reuses the member's film layers), actions, labels and sound events.
Bounds describe geometry and do not impose an implicit clipping rectangle.
Buttons store u16 ID, u8 track-as-menu, bounds, four layer lists in
Up/Over/Down/HitTest order, and conditional actions. Sounds store u16 ID,
asset string, u32 source sample count and rate. Exports store u16 ID and name.

Every retained layer extends the ordinary film layer with u16 clip, button,
field and clip-depth IDs, u32 placement generation, a u8 color-transform
presence flag, optional eight i16 color-transform components (RGBA
multipliers in 8.8 followed by signed additions), and actions. Retained raster
assets are in definition-local coordinates; the full placement matrix and
color transform remain live. Move records preserve placement generation;
removal and replacement produce independent runtime instances.

An action list begins with u16 count. Each action contains u16 one-based
frame (zero for placement/button events), u32 event mask, u8 key code,
u32 byte length, and raw validated AVM1 bytes. Ordered DoInitAction records
are a separate final table: u16 count, then u16 target definition and an
action list containing exactly one initializer with its containing root
frame. These run once in source tag order before normal actions and object
creation, including definitions that are never placed.

The initializer table is followed by u16 Flash font count and that many
nonzero u16 references to the movie's deduplicated text-style table. These
styles retain exact font identity, size, and measured metrics for script-created
fields and statically known TextFormat uses, without inventing visible fields.
Missing font identities or packed size variants fail shared preflight; unknown
dynamic names remain explicit runtime errors.

Each clip's sound-event table begins with u16 count. Records contain u16
frame, sound ID and loop count; u8 SOUNDINFO flags and envelope count; u32
in/out points; and envelope triples (u32 44.1-kHz position, u16 left/right
levels). Out points are inclusive in the file; hosts receive a normalized
half-open sample range. Envelope positions must increase and levels cannot
exceed 32768. Event voices are separate from Director sound channels. N64
packing keeps these assets as uncompressed source-rate PCM, preserving
arbitrary sample boundaries and sample-level envelopes.

Version 8 adds an explicit geometric mask asset to retained raster layers, the DefineEditText UseOutlines flag, cast texture catalogs, and authored palette colors. After all profile-specific member fields, every profile stores a u16 palette color count (0..256), followed by that many u32 RGB colors. A zero count means no CLUT. A partial CLUT remains exact metadata; using it as a stage palette is explicitly blocked until its remaining-color semantics are qualified. Older packages are rejected and must be regenerated. Score channel 5 retains palette commands in spec.member: low 16 signed builtin IDs (-1 Mac, -3 Grayscale, -102 Windows), or the resolved custom palette member ID. Unsupported fade/cycle interpolation fails conversion/preflight.

Version 9 retains native D8.5/D10 film sprites as compact per-channel tracks.
After the expanded Flash frame array, u32 native film flags preserve sound,
loop and geometry settings. A u16 track count is followed by each
track's u16 channel, u16 key count, u32 byte length, and that many delta bytes.
Each delta starts with a u16 zero-based frame and u16 changed-group mask,
followed by its changed groups in ascending bit order:

| Bit | Fields | Encoding |
| --- | --- | --- |
| 0 | member | u32 |
| 1–4 | x, y, width, height (one per bit) | i16 |
| 5 | ink, blend | 2 × u8 |
| 6 | type, flags | 2 × u8 |
| 7 | foreground index, background index | 2 × u8 |
| 8 | thickness, stretch, trails | 3 × u8 |
| 9 | loc_z | i16 |
| 10–11 | foreground/background RGB (one per bit) | u32 |
| 12–13 | rotation/skew (one per bit) | i32 |

Bits 14–15 and a zero mask are invalid. All values are little-endian. Each
track starts from a zero specification; omitted groups retain their previous
value. Clearing a sprite therefore has an explicit delta. Script, tempo,
delay and behavior fields remain zero for these internal film children.
Channels and key frames are strictly increasing. The loader validates every
delta and exact byte consumption. Native tracks and expanded Flash frames
cannot coexist on one member. XY-only movement occupies eight bytes per key;
playback retains decoded state per live instance and replays on backward seeks.
