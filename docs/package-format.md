# Game package format (D64P, version 2)

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
| 4 | 2 | Format version, 2 |
| 6 | 2 | Director version the tables are laid out for: the port's (500, 600, 700, 800, 1000) |
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
u32   member count, then per member:
        u32 id; u16 number, cast, type, width, height; i16 reg x, reg y;
        str name, asset, text;
        u32 samples, rate, loop start, loop end;
        u16 shape, pattern; u8 filled, line width, looping, film count,
        film loop, line direction; film count × str film assets
        extended: u8 editable; u32 binary text length, and when nonzero
          that many bytes plus a NUL (the member text, NULs included);
          u32 source bytes; u8 cue count, then per cue: u32 ms; str name
        modern:   u16 text style, u16 insert style (1-based, 0: none)
        D5:       str video audio; u32 video flags; u32 film sound count
                  (0, or two per film frame), then that many u32 references
        D10:      u8 source Xtra; u8 Flash label count; u8 Flash field count;
                  labels (str name; u16 frame); fields (str name, variable,
                  text; i16 x, y, width, height, left margin, right margin,
                  indent; u8 align, word wrap, multiline; i8 leading;
                  u16 style)
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
