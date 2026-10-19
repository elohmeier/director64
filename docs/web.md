# Standalone web app

`director64 web` builds one game as a static site: the player page, the
engine compiled to WebAssembly with Emscripten, and that game converted from
your own disc image. It is a by-product of the Android TV player
([android.md](android.md)): the conversion is the same, and so is the engine
source. Like an APK, the site contains the converted game. It is for your own
use and must not be redistributed or published.

## Build and serve

```sh
uv run --locked director64 web --game findus-workshop --serve
```

| Option | Effect |
| --- | --- |
| `--reconvert` | Convert the disc image again, even if a current conversion exists |
| `--serve` | Serve the site at `http://127.0.0.1:18064/` after building |
| `--port N` | Serve on another port |
| `--no-build` | Serve the existing build without rebuilding |

A game can be built once it has a port and a browser profile,
`games/<slug>/web.toml` (its save archive's file names, or `saves = false`).
The build needs Docker or a compatible engine (Emscripten 6.0.10, pinned by
digest in `src/director64/web.py` and run without network) for the runtime,
and what the conversion needs: Rust, a host C and C++ compiler, Node with the
locked dependencies and FFmpeg.

The steps:

1. **Convert** (`src/director64/convert.py`, shared with the APK) the disc image under
   `media/<slug>/<source>/` into `build/<slug>/<source>/converted/`, unless a
   conversion by the same converter revision, runtime ABI and game profile is
   already there. The converter is described in
   [architecture.md](architecture.md#conversion).
2. **Validate** the converted package with the production loader
   (`tools/director/validate-package.c`, `runtime/package/package.c`).
3. **Compile** the runtime for the game's Director family with `emcc`
   (`web.compile_command`): the same sources and flags the Android build
   compiles for the NDK, plus the Emscripten glue.
4. **Write** the site to `dist/<slug>/<source>/web/` and a probe launcher
   beside it, `dist/<slug>/<source>/director-web-probe`.

## What the site contains

| File | Content |
| --- | --- |
| `index.html`, `*.js`, `style.css` | The player page (`platforms/web/site/`) |
| `director64.mjs`, `director64.wasm` | The runtime |
| `game.d64p` | The game package ([package-format.md](package-format.md)) |
| `assets/<kind>.bin`, `.json` | Asset packs: images, audio, fonts, streams, data, video and print documents, each with its index; images are stored inflated |
| `monogram.ttf` | The builtin font unstyled text is drawn in (CC0) |
| `print.html`, `print.css` | The print document layout (Löwenzahn 1) |
| `build.json` | Revision, runtime flags and digest, package digest, packs |
| `NOTICES.txt` | The license, [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) and the license texts |
| `manifest.webmanifest` | Name and fullscreen display, so the page can be installed as an app |

A Content Security Policy confines the page to its own origin; nothing is
uploaded. Any static file server works; `--serve` sends `Cache-Control:
no-cache` so a rebuild is what loads.

## Playing

The page loads `build.json`, the package and the packs, and asks for a click
or a button press before starting, because browsers unlock audio only on a
user gesture.

- **Mouse and keyboard.** The mouse points and clicks; the right button is
  the game's second button. From Director 7 on, keys reach the scripts as a
  desktop projector delivers them: the Macintosh virtual key code of the
  physical key and the character the layout types. D5 and D6 scripts
  declare no key handler. A press on an editable field opens a text box over
  it; the game pauses until Enter commits it through `dg_edit_text`
  (20 characters, printable ASCII except the double quote, German letters on
  D10). Mucklas (D8) takes names through keys and types into its field
  directly.
- **Controllers.** Every gamepad the browser reports with the standard
  mapping (a DualShock 4 or DualSense among them) becomes one of four
  players (`site/gamepad.js`). Each pad is passed to the runtime as a
  controller sample (`d64_pad`, `runtime/interaction/input.h`), and
  `runtime/interaction/pointer.c` turns it into that player's cursor; whoever
  presses or points holds Director's one mouse. ✕ (or the touchpad click)
  clicks, ○ is the second button, Options is Start, the left stick and D-pad
  move the cursor (L1 held: fine movement), the right stick gives the
  direction buttons some games use, and PS or Share opens the menu. Moving
  the mouse takes the pointer back. On an editable field ✕ opens an
  on-screen keyboard (`site/keyboard.js`). The per-game controller layers
  (Mucklas' race steering, Willy's notices, Löwenzahn's print dismissal) are
  the same as on Android TV.
- **Display.** The stage is 640×480, or 800×600 for a game authored at that
  size. "Sharp" scaling (the default) enlarges it by the next whole factor
  without filtering and lets the browser shrink the rest; "Smooth" filters.
  The Fullscreen button keeps the menu, notices and on-screen keyboard
  visible and, with the keyboard lock, passes Escape to the game. A screen
  wake lock is held while a game runs.
- **Menu.** It pauses the game and holds volume and mute, fullscreen,
  scaling, diagnostics (also `?debug`), save export and import, and the
  controls. Preferences live in `localStorage`; texts come in English and
  German (`site/strings.js`, by browser language or `?lang=`).
- **Printing.** Löwenzahn 1's print stamps open the browser's print dialog
  with the chapter's recovered text and artwork laid out on an A4 page
  (`print.html`), then dismiss the game's print pause.

The service clock runs at 60 Hz from `requestAnimationFrame` time, at most
four ticks per animation frame with twelve ticks of backlog, as on Android;
longer stalls drop time rather than rushing the score. A hidden page stops
game time and audio together.

## Saves

The runtime stores a game's saves in its 128 KiB save image
([architecture.md](architecture.md#runtime-hosts)); the game's own archive
code commits to it. After each
commit the page writes the whole image to the origin-private file system
(OPFS) under `director64/<slug>/saves/` and shows *Saved* only once that
write has closed (`site/saves.js`). A failed write shows an error and keeps
the previous generation. The menu exports the image as a file and imports
one; the current save is kept as a backup until the imported one boots, and
can be restored if it fails to load. The page asks for persistent storage
and warns when the browser has not granted it. An unreadable save stops the
game without changing it.

Saves belong to the origin: serve the site from the same host and port to
keep them.

## Browser requirements

A current desktop Chrome or Edge. The page checks for the origin-private
file system with writable file handles and for WebAssembly before it
starts. Video (VP9) decodes with WebCodecs (`VideoDecoder`); sounds (WAV,
Ogg Opus, MP3) decode through Web Audio.

## The probe

`director-web-probe` runs `platforms/web/node/probe.mjs` over the site's
runtime under Node and speaks the probe line protocol (`MOVIE rpc`, then one
command per line): `state`, `step`, `pad`, `point N` and the rest. It starts
at the game's entry movie only. It renders by default; `--state-only` skips
the compositor.

`director64 fuzz`, `director64 parity` and `director64 journey` drive it
([fuzzing.md](fuzzing.md)); each needs a web build first. In the running
page, `window.director64.rpc("state")` returns the same state line and
`rpc("point N")` a hittable point of sprite N.

## What runs where

| Piece | Responsibility |
| --- | --- |
| `platforms/web/site/` | The page: shell and menu (`app.js`), the player (`player.js`: clock, input, presentation, printing), sound (`audio.js`, `flash-audio.js`), video (`video.js`), saves (`saves.js`), controllers (`gamepad.js`, `keyboard.js`), fullscreen and wake lock (`screen.js`), preferences and texts |
| `platforms/web/web_runtime.c` | Engine instance, platform callbacks, save device, the service step and the probe protocol |
| `platforms/web/compositor.c` | Software compositor into an RGBA framebuffer: draw order, inks, blend, stretching, shapes, text, film loops and the cursor; shared with Android |
| `platforms/web/webp_image.c` | Decodes images stored as lossless WebP, with the vendored libwebp decoder |
| `platforms/web/web_main.c`, `library.js` | Emscripten glue: the runtime's imports forward to the page |
| `platforms/web/node/probe.mjs` | The probe protocol over the same Wasm module |
| `runtime/package/package.c` | Validates the game package and builds the engine's movie tables from it |
| `games/<slug>/web.toml` | The game's save file names |
