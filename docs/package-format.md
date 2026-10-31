# Game package format (D64P, version 9)

`game.d64p` holds everything a converted game needs besides the runtime:
each movie's handler bytecode and name pools, the symbol table, global
slots, and each movie's scene tables (casts, members, score, labels,
palette). The web and Android players load the same file, so one runtime
build serves every game of its Director family.

`compiler/src/package.rs` writes it; `runtime/package/package.c` validates
and loads it into the structures of `runtime/lingo/lingo_runtime.h` and
`runtime/director/director.h`. Integers are little-endian with stated
widths; references are offsets or indexes the loader bounds-checks. Nothing
is a serialized C structure or pointer.

## Header

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `D64P` |
| 4 | 2 | Format version, 9 |
| 6 | 2 | Director version the tables are laid out for: the port's (500, 600, 700, 800, 1000; a Director 8.5 port is laid out as 1000) |
| 8 | 2 | Profile flags; bit 0 is extended D6; bit 1 selects the full engine member layout |
| 10 | 2 | Section count |
| 12 | 16 | ABI digest: the first 16 bytes of SHA-256 over `runtime/lingo/names.txt` followed by `runtime/lingo/lingo_bytecode.h` |
| 28 | 4 | Reserved, zero |
| 32 | 12 × n | Section table: tag (four ASCII bytes), offset, length (u32 each) |

A runtime is built for one family (`runtime/lingo/family.h`) and refuses a
package laid out for another or with a different ABI digest: name ids and
opcodes are positions in those two files.

The full engine flag requires the extended flag. It enables rich member tails
(video, live film channels and Flash tables) on Director 6–8 without changing
the source score decoder or language family. All other flag bits are rejected.

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

The loader checks every count against the remaining section, every string
against the pool, every code range against `CODE`, every handler order,
bucket, frame and member index against its table, and that movie ids count
1..n in record order. A failed check rejects the whole package.

## Palette colors

After all profile-specific member fields, every member stores a u16 palette
color count (0..256) and that many u32 RGB colors; zero means no CLUT. A
partial CLUT is kept as metadata but cannot be used as a stage palette.
Score channel 5 keeps palette commands in `spec.member`: the low 16 bits are
a signed builtin id (-1 Mac, -3 Grayscale, -102 Windows) or the resolved
custom palette member id. Unsupported fade/cycle interpolation fails
conversion.

## Flash members

Flash members additionally store, in this order:

- u32 intrinsic frame rate (16.16); u16 stream position count and that many
  u32 PCM sample offsets (count is 0 or the logical frame count;
  `0xFFFFFFFF` marks a frame without a stream block). The video-audio
  asset, samples and rate fields carry the decoded stream.
- u8 playback mode (0 Normal, 1 Fixed, 2 Lock-Step), u32 fixed frame rate
  (16.16), u8 direct-to-stage, u8 original SWF version (selects AVM1
  version-dependent behavior).
- Four u16 counts, then the clip, button, sound and export tables. Character
  ids share one namespace with fields; id 0 is the root clip.
  - Clip: u16 id, frame count; four signed 16.16 local bounds; its frame
    layers (the root reuses the member's film layers), actions, labels and
    sound events. Bounds do not clip.
  - Button: u16 id, u8 track-as-menu, bounds, four layer lists
    (Up/Over/Down/HitTest), conditional actions.
  - Sound: u16 id, asset string, u32 source sample count and rate.
  - Export: u16 id and name.
- DoInitAction table: u16 count, then u16 target definition and an action
  list with exactly one initializer and its root frame. These run once in
  source order before normal actions and object creation.
- u16 Flash font count and that many nonzero u16 references into the
  movie's text-style table (fonts for script-created fields).

A retained layer extends the film layer with u16 clip, button, field and
clip-depth ids, u32 placement generation, a u8 color-transform flag with
optional eight i16 components (RGBA 8.8 multipliers, then signed additions),
and actions. Raster assets are in definition-local coordinates; the
placement matrix and color transform stay live.

An action list is a u16 count of actions: u16 one-based frame (0 for
placement and button events), u32 event mask, u8 key code, u32 byte length,
and the validated AVM1 bytes.

A clip's sound-event table is a u16 count of records: u16 frame, sound id,
loop count; u8 SOUNDINFO flags and envelope count; u32 in/out points
(inclusive out point); envelope triples (u32 44.1 kHz position, u16
left/right levels, at most 32768, positions increasing). Event voices are
separate from Director sound channels.

## Native film tracks

D8.5/D10 film sprites are stored as compact per-channel tracks after the
expanded Flash frame array: u32 native film flags (sound, loop, geometry),
then a u16 track count; each track is u16 channel, u16 key count, u32 byte
length and that many delta bytes. A delta is a u16 zero-based frame and a
u16 changed-group mask, then the changed groups in ascending bit order:

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

Bits 14–15 and a zero mask are invalid. Each track starts from a zero
specification; omitted groups keep their previous value. Channels and key
frames strictly increase, and the loader requires exact byte consumption.
Script, tempo, delay and behavior fields stay zero for film children. Native
tracks and expanded Flash frames cannot coexist on one member.

Optional `VECT` records retain editable straight vector polygons without
changing the `MOVI` layout. The section begins with a u32 record count. Each
record stores member ID u32, vertex count u16 (2..1024), flags u8 (fill 1,
closed 2), reserved zero u8, stroke width f64 (0..255), stroke RGB u32 and fill
RGB u32 (each 0xRRGGBB), followed by horizontal/vertical f64 pairs. Coordinates
must be finite and within +/-65535. Missing, repeated or non-vector member IDs
are rejected. Valid empty editor templates, compound paths separated by
`[#newCurve]`, and Bezier vertices with `handle1`/`handle2` retain their original
static SWF raster rather than an invented editable polygon. A package without
this section retains its static raster artwork;
attempting a live vertex edit without retained metadata fails explicitly.

The optional `TRAN` section preserves transition cast descriptors without
changing `MOVI`: u32 count, then count × (u32 member id; u16 duration in
milliseconds; u8 effect number, chunk size, changed-area flag; three zero
reserved bytes). The area flag is the inverse of bit 0 in the source flags.
The loader rejects missing/non-transition identities, duplicate descriptors,
effects above 52, invalid area flags and trailing bytes. A used transition
member without its converted descriptor fails with an actionable diagnostic.

For native CASt button members (type7), the existing `MOVI` shape field carries
1 push button, 2 checkbox or 3 radio button, and filled carries the initial
checked boolean. Shape zero retains the descriptor-absent legacy field behavior.
The converter reads the BE16 button kind following the 28-byte classic text
descriptor; checked state starts false, as corroborated with original10.1 native
buttons. These fields keep their existing meanings on other member types. The
loader rejects button kinds above3 and checked values above1; no layout or Xtra
identity changes are involved.

The optional `RICH` section preserves text formatting alongside the existing
single-style `MOVI` field descriptor. It starts with u32 member count. Each
member has u32 id, u16 run count, u16 reserved zero, followed by that many
16-byte runs:

```
u32 UTF-16 code-unit offset
u8  font id, point size, face flags, alignment
i16 ascent, line height
u32 RGB color (0xRRGGBB)
```

Face flags are bold 1, italic 2, underline 4 and strikeout 8. Alignment is left 0,
center 1, right 2 or full justification 3. Full justification pads interword
spaces on wrapped rows and leaves the paragraph's final row left aligned.
A nonempty table starts at offset zero, strictly increases,
and has at most 4096 runs. Offsets may reach the end of the text, measured in
UTF-16 units to match the browser and Android text APIs. Size zero preserves the
stored Director value and renders Paige's default 12-point glyphs; font assets
and measurements use that effective size. Line height is nonnegative. The loader
independently checks identities, supported text member type (3 or 7), counts,
reserved bits, ordering, ranges and trailing bytes. Director 5 accepts an empty
`RICH` section and rejects nonempty run data.
Missing `RICH` retains the existing single-style rendering contract. `VECT`
and `TRAN` retain their previous layouts and ordering independence.

The optional `RSPC` section preserves nonzero authored character spacing without
changing the 16-byte `RICH` runs. A u32 count is followed by 12-byte records:
u32 member ID, u32 UTF-16 run offset, i16 pixel spacing, and u16 reserved zero.
Each record must target an existing `RICH` run exactly once. Zero spacing,
missing targets, duplicate targets, nonzero reserved words and trailing bytes
are rejected; absence retains zero spacing. Paige's signed 16.16 `characterExtra`
is accepted only when it represents an integral pixel value within i16 range;
other source values fail conversion with member and run context.

The optional `TBOX` section retains classic CASt field/button presentation
without changing `MOVI` or `RICH`. It starts with u32 count and count × 12-byte
records: u32 member ID; u8 box type (1 adjust, 2 fixed, 3 scroll, 4 limit), margin,
border, box shadow, word-wrap boolean; then three bytes of RGB background.
Fields and buttons are the only valid targets. The loader checks profile
support, identities, duplicate targets, type/boolean ranges and exact extent.
Absent metadata retains the runtime's established defaults. Source recovery
uses the actual CASt bytes; Paige paragraph indents are separate formatting
and are not guessed as field margins.

Native button members (type 7) use the existing type-specific `shape` slot for
control kind: 1 push button, 2 checkbox, 3 radio button. `filled` stores the
initial checked state (0 or 1). Shape zero retains the legacy field contract.
The source kind is the big-endian u16 after the 28-byte classic text descriptor;
recovery validates all three kinds. This adds no section or member-layout bytes.
