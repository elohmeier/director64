# Third-party notices

Director64 is MIT-licensed (see [LICENSE](LICENSE)), except for the files
listed there that carry MPL-2.0 or LGPL-2.1-or-later headers. This file lists
third-party code that the repository builds on, that the converter runs and
that the Android TV player and the standalone web app ship. `director64
android build` and `director64 web` copy this file, followed by the license
texts in [licenses/](licenses), into the APK and the site as `NOTICES.txt`.

No game data is part of this repository. Games are converted from the user's
own disc image on the user's machine; the APKs and web builds that contain
them stay with the user.

## Converter and players

The converter runs locally: the Rust converter `director64-aot`, ProjectorRays
under Node, and host tools built into `build/tools/` (the WebP packer
`d64-webp` and libdragon's `mkfont` and `mksprite`). The players ship the
runtime (`director64.wasm` in the web app, `lib<game>.so` in the APK) and the
builtin font.

| Component | Used for | License |
| --- | --- | --- |
| [ProjectorRays](https://github.com/ProjectorRays/ProjectorRays) 1.1.1 (npm `projectorrays`), unmodified | Director file parser in the converter (`node_modules/projectorrays`) | MPL-2.0 — [licenses/MPL-2.0.txt](licenses/MPL-2.0.txt) |
| [mpg123](https://www.mpg123.de/) (libmpg123), linked into the ProjectorRays build | MP3 decoding inside the parser | LGPL-2.1 — [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt); source at https://www.mpg123.de/, build recipe in the ProjectorRays repository |
| [zlib](https://zlib.net/) 1.3.1, linked into the ProjectorRays build | Decompression inside the parser | zlib (below); Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler |
| Director64 score decoder (`compiler/src/convert/score.rs`, a port of a ProjectorRays score-recovery extension) | Score recovery in the converter (`director64-aot`) | MPL-2.0; source in this repository |
| Director64 Cinepak decoder (`compiler/src/convert/cinepak.rs`), derived from [FFmpeg](https://ffmpeg.org/)'s `libavcodec/cinepak.c`, Copyright (C) 2003 The FFmpeg project | Video conversion in the converter (`director64-aot`) | LGPL-2.1-or-later — [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt); source in this repository |
| [Symphonia](https://github.com/pdeljanov/Symphonia/tree/v0.5.5) 0.5.5 (core, metadata and MP3 bundle) | MP3 cast decoding in the converter (`director64-aot`) | MPL-2.0 — [licenses/MPL-2.0.txt](licenses/MPL-2.0.txt); corresponding source at the versioned upstream link |
| [Ruffle NihAV VP6 subset](https://github.com/ruffle-rs/nihav-vp6/tree/12d0bdf10a85641dc2de3d6900c747894431ed77), nihav_core, nihav_codec_support, nihav_duck | Embedded SWF VP6 decoding in the converter (`director64-aot`) | MIT — [licenses/nihav-vp6.txt](licenses/nihav-vp6.txt); Copyright (c) 2021 Kostya Shishkov |
| Rust crates: serde_json, serde_core, indexmap, hashbrown, equivalent, itoa, sha2, digest, block-buffer, crypto-common, typenum, cfg-if, miniz_oxide, adler2, jpeg-decoder | The converter (`director64-aot`) | Used under Apache-2.0 (each also offers MIT) — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| Rust crates: generic-array (Copyright (c) 2015 Bartłomiej Kamiński), zmij (Copyright (c) David Tolnay) | The converter (`director64-aot`) | MIT (below) |
| Rust crates: cpufeatures, libc, serde | The converter (`director64-aot`) | Used under Apache-2.0 (each also offers MIT) — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| Rust crates: serde_derive, proc-macro2, quote, syn, unicode-ident, version_check | Build-time only (macros and build scripts), not part of the converter binary | Used under Apache-2.0 (each also offers MIT) — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt); unicode-ident also under the [Unicode License V3](licenses/unicode.txt) |
| Rust crate: memchr | The converter (`director64-aot`) | Used under the Unlicense |
| Rust crates: arrayvec, bitflags, bytemuck, core_detect, lazy_static, log, multiversion_no_op, rustversion, scopeguard, simdutf8 | MP3 conversion dependencies in `director64-aot` | Used under Apache-2.0 (each also offers MIT) — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| Rust crate: encoding_rs | MP3 metadata in `director64-aot` | Apache-2.0 and BSD-3-Clause — [licenses/encoding_rs.txt](licenses/encoding_rs.txt), [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| Rust crate: cab 0.6.0 | Read original system-font installer cabinets as data | MIT — [licenses/cab.txt](licenses/cab.txt); Copyright (c) 2017 Matthew D. Steele |
| Rust crates: flate2, crc32fast, lzxd, time, time-core, deranged, num-conv, powerfmt | Cabinet decoding dependencies | Used under Apache-2.0 (each also offers MIT) — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| Rust crate: byteorder | Cabinet decoding dependency | Used under the Unlicense |
| Rust crate: simd-adler32 | Cabinet decoding dependency | MIT — [licenses/simd-adler32.txt](licenses/simd-adler32.txt) |
| [libdragon](https://github.com/DragonMinded/libdragon) mkfont and mksprite, from the pinned [fork](https://github.com/elohmeier/libdragon/tree/findus64) in `third_party/libdragon` | Font rasterizing and glyph measurement in the converter (host tools built into `build/tools/libdragon/`) | Unlicense |
| [FreeType](https://freetype.org/), linked into mkfont | Font rasterizing | FreeType License — [licenses/FTL.txt](licenses/FTL.txt). Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved. |
| [plutosvg / plutovg](https://github.com/sammycage/plutosvg), linked into mkfont | SVG glyphs | MIT (below); Copyright (c) 2020-2025 Samuel Ugochukwu |
| [LodePNG](https://github.com/lvandeve/lodepng), linked into mksprite | PNG decoding | zlib (below); Copyright (c) 2005-2021 Lode Vandevenne |
| [LZ4](https://github.com/lz4/lz4), linked into mkfont and mksprite | Asset compression | BSD-2-Clause (below); Copyright (C) 2011-2020, Yann Collet |
| [apultra](https://github.com/emmanuel-marty/apultra), linked into mkfont and mksprite | Asset compression | zlib (below); Copyright (C) 2019 Emmanuel Marty |
| [libdivsufsort](https://github.com/y-256/libdivsufsort), part of apultra | Suffix sorting | MIT (below); Copyright (c) 2003-2008 Yuta Mori |
| [Shrinkler](https://github.com/askeksa/Shrinkler) compressor, linked into mkfont and mksprite | Asset compression | Shrinkler license — [licenses/Shrinkler.txt](licenses/Shrinkler.txt); Copyright 1999-2022 Aske Simon Christensen |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) 1.5.0 (`third_party/libwebp`), unmodified | Lossless WebP images: the encoder in the converter (`d64-webp`), the decoder in the runtime (the web app's `director64.wasm` and the Android TV player's library) | BSD-3-Clause — [licenses/libwebp.txt](licenses/libwebp.txt), with Google's patent grant (`third_party/libwebp/PATENTS`); Copyright (c) 2010, Google Inc. |
| [Emscripten](https://emscripten.org/) runtime support code | The web app's runtime (`director64.mjs`, `director64.wasm`) | MIT (below); Copyright (c) 2010-2014 Emscripten authors |
| [PL_MPEG](https://github.com/phoboslab/pl_mpeg) at `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023` (`third_party/pl_mpeg`) | Independent MPEG-1 video and MP2 audio decoder in the tests (`tests/native/test_mpeg_decoder.c`, and `tests/native/mpeg_tool.c` for `tests/test_mpeg_picture.py`); not shipped | MIT (below); Dominic Szablewski |
| Droid Sans, from libdragon's font gallery example (`third_party/libdragon`) | Substitute for unembedded Director system fonts, converted into the game's fonts | Apache-2.0 — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt); Copyright © 2007 Google |
| Microsoft Arial 2.82, from the original Core Fonts `arial32.exe` installer | Exact-family font for live Flash fields during local conversion | Proprietary Core Fonts EULA, included in the unchanged installer. The build downloads the original redistributable; no font is committed here. Redistribution is restricted to its original installer form and filename; see [Microsoft's Core Fonts FAQ](https://corefonts.sourceforge.net/faq8.htm). Extracted fonts and generated game assets stay local. |
| [monogram](https://datagoblin.itch.io/monogram) by datagoblin, from libdragon's font gallery example (`third_party/libdragon`) | Builtin font for unstyled text, shipped as `monogram.ttf` in the APK and the web app | CC0 |

The LGPL-2.1 components are distributed with complete corresponding source:
mpg123 at the URL above, the Cinepak decoder in this repository. The web
app's runtime can be rebuilt from this repository with `director64 web`, and
every APK with `director64 android build`, which permits relinking a modified
version.

## libdragon

The pinned [libdragon fork](https://github.com/elohmeier/libdragon/tree/findus64)
in `third_party/libdragon` (Unlicense, with bundled components under their own
licenses; see that repository) supplies the two fonts above and the mkfont and
mksprite tools the converter builds as host tools. Nothing else of it is
built.

## Behavioral references

[ScummVM](https://github.com/scummvm/scummvm) and ProjectorRays were consulted
as references for Director file formats and runtime behavior. No ScummVM code
is included.

---

## MIT License

Applies to the components marked MIT above, with their copyright notices.

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## BSD 2-Clause License

Applies to LZ4, with its copyright notice.

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

## zlib License

Applies to zlib, LodePNG and apultra, with their copyright notices.

```
This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Unicode case data

`runtime/director/avm1_case.h` contains generated Unicode 3.2 case mappings,
used by the ActionScript string library. The generator records SHA-256 hashes
of UnicodeData-3.2.0.txt from Unicode.org.
Copyright Unicode, Inc.; distributed under the [Unicode License V3](licenses/unicode.txt).
