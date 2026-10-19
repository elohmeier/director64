# Löwenzahn Adventskalender

Experimental Director 8.5 port of the locally supplied German disc, for the
Android TV player and the standalone web app. Director 8.5 runs as the D10
runtime family with extended behaviors and `-DDIRECTOR64_D85`
(`runtime/lingo/family.h`). The movies are Shockwave (Afterburner) files.

```sh
uv run --locked director64 android build --game loewenzahn-advent --abi armeabi-v7a
uv run --locked director64 android install --game loewenzahn-advent --serial <device>
uv run --locked director64 web --game loewenzahn-advent --serve
```

There is no launcher projector to convert: `START` is the entry movie. The
preferences file saves as `Adventskalender.usr` in the save archive.

Videos are MPEG-1 movies played through Tabuleiro's DirectMedia Xtra. Each
member names its file by the authoring machine's path; the port binds it to
the disc's 8.3 file (`compiler/src/ports.rs`). The conversion transcodes
them with FFmpeg to VP9, with their sound as WAV or Ogg Opus; the Android TV
and web players share that conversion. The Mac QuickTime
cast (`videoM`) is pinned as deferred: the runtime answers `the machineType`
as Windows, so it is never used.

`getRegOptimal`, the `optgraph.dll` copy check, answers its serial
(`compiler/src/ports.rs`, `optgraph_compatibility`).

The calendar follows the device's date (`the systemDate`): before
December it counts down to the first door, as the original does. The
paint box's file dialogs are not ported.

Checked with the web build's probe: the start, the choice screen and the December countdown, without script errors.

Known gaps:

- Members listed under `deferred_conversions` in `host/source-policy.json`
  draw nothing (a few shapes, film loops, Flash timelines over 128 frames).
- Nothing has been compared against the original projector.
