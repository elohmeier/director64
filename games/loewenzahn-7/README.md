# Löwenzahn 7

Experimental Director 8.5 port of the locally supplied German disc, for the
browser player and Android TV (no N64 build). Director 8.5 runs as the D10
runtime family with extended behaviors and `-DDIRECTOR64_D85`
(`runtime/lingo/family.h`). The movies are Shockwave (Afterburner) files.

```sh
uv run --locked director64 repair-media --game loewenzahn-7 --replacement /path/to/VI_05_01.MPG
uv run --locked director64 android build --game loewenzahn-7 --abi armeabi-v7a
uv run --locked director64 android install --game loewenzahn-7 --serial <device>
```

The selected image has damaged MPEG data. `repair-media` creates a separate,
hash-pinned ISO and receipt while retaining the original. It requires the
independently recovered long clip with SHA-256
`5513bc95319ec238310f694f8ad52117814f66503fe76e12f6ef44cd2bde1ead` and reconstructs
one 40 ms picture in each of five short clips. The prepared copy is selected
automatically once present. The recipe is in `host/media-repair.json`; the
ordinary importer and strict video/audio checks still apply.

There is no launcher projector to convert: `START` is the entry movie. The
preferences file saves as `LZ7.usr` in the save archive.

Videos are MPEG-1 movies played through Tabuleiro's DirectMedia Xtra. Each
member names its file by the authoring machine's path; the port binds it to
the disc's 8.3 file (`compiler/src/ports.rs`). The Android export transcodes
them with FFmpeg to VP9 and WAV/Opus. The browser importer uses the shared MPEG-1
decoder and produces indexed VP8 with aligned audio. The Mac QuickTime
cast (`videoM`) is pinned as deferred: the runtime answers `the machineType`
as Windows, so it is never used.

`getRegOptimal`, the `optgraph.dll` copy check, answers its serial
(`compiler/src/ports.rs`, `optgraph_compatibility`).

Checked in the browser runtime's probe: the piggy-bank login, the intro and the roof hub, without script errors.

Original-projector parity and current physical Android gameplay checks remain
pending.
