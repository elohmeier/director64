# Third-party notices

Director64 is MIT-licensed (see [LICENSE](LICENSE)), except for the files
listed there that carry MPL-2.0 or LGPL-2.1-or-later headers. This file lists
third-party code that the repository builds on and that the browser player
ships. `director64 web` copies this file, followed by the license texts in
[licenses/](licenses), into the site as `NOTICES.txt`.

No game data is part of this repository or of the browser player. Games are
converted from the user's own disc image on the user's machine.

## Shipped in the browser player

| Component | Used for | License |
| --- | --- | --- |
| [ProjectorRays](https://github.com/ProjectorRays/ProjectorRays) 1.1.1 (npm `projectorrays`), unmodified | Director file parser (`import/projectorrays.wasm`) | MPL-2.0 — [licenses/MPL-2.0.txt](licenses/MPL-2.0.txt) |
| [mpg123](https://www.mpg123.de/) (libmpg123), linked into the ProjectorRays build | MP3 decoding inside the parser | LGPL-2.1 — [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt); source at https://www.mpg123.de/, build recipe in the ProjectorRays repository |
| [zlib](https://zlib.net/) 1.3.1, linked into the ProjectorRays build | Decompression inside the parser | zlib (below); Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler |
| Director64 score decoder (`compiler/src/convert/score.rs`, from `tools/projectorrays/`) | Score recovery in `import/convert.wasm` | MPL-2.0; source in this repository |
| Director64 Cinepak decoder (`compiler/src/convert/cinepak.rs`), derived from [FFmpeg](https://ffmpeg.org/)'s `libavcodec/cinepak.c`, Copyright (C) 2003 The FFmpeg project | Video conversion in `import/convert.wasm` | LGPL-2.1-or-later — [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt); source in this repository |
| Rust crates: serde_json, serde_core, indexmap, hashbrown, equivalent, itoa, sha2, digest, block-buffer, crypto-common, typenum, cfg-if, miniz_oxide, adler2 | `import/convert.wasm` | Used under Apache-2.0 (each also offers MIT) — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) |
| Rust crates: generic-array (Copyright (c) 2015 Bartłomiej Kamiński), zmij (Copyright (c) David Tolnay) | `import/convert.wasm` | MIT (below) |
| Rust crate: memchr | `import/convert.wasm` | Used under the Unlicense |
| [libdragon](https://github.com/DragonMinded/libdragon) mkfont and mksprite | Font and sprite packing (`import/mkfont.wasm`, `import/mksprite.wasm`) | Unlicense |
| [FreeType](https://freetype.org/), linked into mkfont | Font rasterizing | FreeType License — [licenses/FTL.txt](licenses/FTL.txt). Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved. |
| [plutosvg / plutovg](https://github.com/sammycage/plutosvg), linked into mkfont | SVG glyphs | MIT (below); Copyright (c) 2020-2025 Samuel Ugochukwu |
| [LodePNG](https://github.com/lvandeve/lodepng), linked into mksprite | PNG decoding | zlib (below); Copyright (c) 2005-2021 Lode Vandevenne |
| [LZ4](https://github.com/lz4/lz4), linked into mkfont and mksprite | Asset compression | BSD-2-Clause (below); Copyright (C) 2011-2020, Yann Collet |
| [apultra](https://github.com/emmanuel-marty/apultra), linked into mkfont and mksprite | Asset compression | zlib (below); Copyright (C) 2019 Emmanuel Marty |
| [libdivsufsort](https://github.com/y-256/libdivsufsort), part of apultra | Suffix sorting | MIT (below); Copyright (c) 2003-2008 Yuta Mori |
| [Shrinkler](https://github.com/askeksa/Shrinkler) compressor, linked into mkfont and mksprite | Asset compression | Shrinkler license — [licenses/Shrinkler.txt](licenses/Shrinkler.txt); Copyright 1999-2022 Aske Simon Christensen |
| [Emscripten](https://emscripten.org/) runtime support code | All C/C++ WebAssembly modules | MIT (below); Copyright (c) 2010-2014 Emscripten authors |
| Droid Sans, from libdragon's font gallery example | Substitute for unembedded Director system fonts | Apache-2.0 — [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt); Copyright © 2007 Google |
| [monogram](https://datagoblin.itch.io/monogram) by datagoblin | Console debug font | CC0 |

The LGPL-2.1 components are distributed with complete corresponding source:
mpg123 at the URL above, the Cinepak decoder in this repository. Every
WebAssembly module can be rebuilt from this repository with
`director64 web`, which permits relinking a modified version.

## Used to build N64 ROMs

ROMs are built against the pinned [libdragon fork](https://github.com/elohmeier/libdragon/tree/findus64)
in `third_party/libdragon` (Unlicense, with bundled components under their own
licenses; see that repository). A built ROM contains assets converted from the
user's disc and must not be redistributed.

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
