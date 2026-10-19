# Standalone web app

`director64 web` builds one game as a static site: the player page, the
engine compiled to WebAssembly with Emscripten, and the game converted from
your own disc. It is a by-product of the Android TV player
([android.md](android.md)): same conversion, same engine sources. Like an
APK, the site contains the converted game and must not be published.

```sh
uv run --locked director64 web --game findus-workshop --serve
```

| Option | Effect |
| --- | --- |
| `--reconvert` | Convert the disc image again |
| `--serve` | Serve the site at `http://127.0.0.1:18064/` after building |
| `--port N` | Serve on another port |
| `--no-build` | Serve the existing build |

The build converts the game ([architecture.md](architecture.md#conversion)),
validates the package with the C loader, compiles the runtime with `emcc`
in a pinned Emscripten container (no network), and writes the site to
`dist/<slug>/<source>/web/` with a probe launcher beside it,
`dist/<slug>/<source>/director-web-probe`.

## Site contents

| File | Content |
| --- | --- |
| `index.html`, `*.js`, `style.css` | The player page (`platforms/web/site/`) |
| `director64.mjs`, `director64.wasm` | The runtime |
| `game.d64p` | The game package ([package-format.md](package-format.md)) |
| `assets/<kind>.bin`, `.json` | Asset packs (images, audio, fonts, streams, data, video, print) |
| `monogram.ttf` | Builtin font for unstyled text |
| `print.html`, `print.css` | Print layout (Löwenzahn 1) |
| `build.json` | Revision, runtime flags and digests, packs |
| `NOTICES.txt` | License and third-party notices |
| `manifest.webmanifest` | Installable fullscreen app |

Any static file server works; `--serve` sends `Cache-Control: no-cache`.

## Playing

Requires a current desktop Chrome or Edge (origin-private file system with
writable handles, WebAssembly, WebCodecs for VP9 video). The page waits for
a click or button press so audio can start.

- **Mouse and keyboard.** The right button is the second button. From
  Director 7 on, keys reach the scripts as on a desktop projector. Clicking
  an editable field opens a text box; the game waits until Enter.
- **Controllers.** Each standard-mapping gamepad becomes one of four players
  with its own cursor (`site/gamepad.js`), mapped as on Android TV. On an
  editable field ✕ opens an on-screen keyboard.
- **Display.** 640×480, or 800×600 for games authored at that size. "Sharp"
  scaling (default) uses whole factors; "Smooth" filters.
- **Menu.** Pauses the game; volume, fullscreen, scaling, diagnostics (also
  `?debug`), save export/import, controls. English and German texts
  (`?lang=`).
- **Printing.** Löwenzahn 1's print stamps open the browser's print dialog
  with the chapter laid out on A4.

## Saves

After each commit the page writes the 128 KiB save image
([architecture.md](architecture.md#saves-and-files)) to the origin-private
file system under `director64/<slug>/saves/`, and shows *Saved* once the
write has closed. The menu exports and imports the image; the current save
is kept as a backup until an imported one boots. Saves belong to the origin:
serve from the same host and port to keep them.

## Probe

`director-web-probe` runs `platforms/web/node/probe.mjs` over the site's
runtime under Node and speaks a line protocol (`state`, `step`, `pad`,
`point N`, ...); `--state-only` skips rendering. `director64 fuzz`, `parity`
and `journey` drive it ([fuzzing.md](fuzzing.md)). In the page,
`window.director64.rpc("state")` returns the same state line.

## Source layout

| Piece | Responsibility |
| --- | --- |
| `platforms/web/site/` | The page: shell and menu, player clock and input, audio, video, saves, controllers, preferences |
| `platforms/web/web_runtime.c` | Engine instance, platform callbacks, save device, service step, probe protocol (shared with Android) |
| `platforms/web/compositor.c` | Software compositor (shared with Android) |
| `platforms/web/webp_image.c` | Lossless WebP image decoding |
| `platforms/web/web_main.c`, `library.js` | Emscripten glue |
