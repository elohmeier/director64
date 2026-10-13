# Löwenzahn 8

Experimental Director 8.5 port of the locally supplied German disc, for the
browser player and Android TV (no N64 build). Director 8.5 runs as the D10
runtime family with extended behaviors and `-DDIRECTOR64_D85`
(`runtime/lingo/family.h`). The movies are Shockwave (Afterburner) files.

```sh
uv run --locked director64 android build --game loewenzahn-8 --abi armeabi-v7a
uv run --locked director64 android install --game loewenzahn-8 --serial <device>
```

There is no launcher projector to convert: `START` is the entry movie. The
preferences file saves as `LZ8.usr` in the save archive.

Videos are MPEG-1 movies played through Tabuleiro's DirectMedia Xtra. Each
member names its file by the authoring machine's path; the port binds it to
the disc's 8.3 file (`compiler/src/ports.rs`). The Android export transcodes
them with FFmpeg to VP8 and WAV; the browser importer has no MPEG-1 decoder,
so there the clips have no length and the game skips them. The Mac QuickTime
cast (`videoM`) is pinned as deferred: the runtime answers `the machineType`
as Windows, so it is never used.

`getRegOptimal`, the `optgraph.dll` copy check, answers its serial
(`compiler/src/ports.rs`, `optgraph_compatibility`).

Löwenzahn 8 keeps its preferences through the vList Xtra; the runtime
stores them as Lingo literals in the save archive. The Tamagotchi dog's
animations are linked Shockwave movies played inside a sprite (cast type 9),
which draw nothing yet.

Checked in the browser runtime's probe: the boat login, the intro video, the Bauwagen hub, the Tamagotchi setup and the Videomat, without script errors.

Known gaps:

- Members listed under `deferred_conversions` in `host/source-policy.json`
  draw nothing (a few shapes, film loops, Flash timelines over 128 frames).
- Nothing has been compared against the original projector.
