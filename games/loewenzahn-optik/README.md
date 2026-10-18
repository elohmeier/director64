# 25 Jahre Löwenzahn: Laser, Linsen, Flaschenzüge

Experimental Director 8.5 port of the locally supplied German disc, for the
browser player and Android TV (no N64 build). Director 8.5 runs as the D10
runtime family with extended behaviors and `-DDIRECTOR64_D85`
(`runtime/lingo/family.h`). The movies are Shockwave (Afterburner) files.

```sh
uv run --locked director64 repair-media --game loewenzahn-optik
uv run --locked director64 android build --game loewenzahn-optik --abi armeabi-v7a
uv run --locked director64 android install --game loewenzahn-optik --serial <device>
```

The selected image contains filesystem journal data overwriting one MPEG.
`repair-media` creates a separate, hash-pinned ISO and receipt while retaining
the original. The recipe in `host/media-repair.json` holds pictures for a total
of 3.32 seconds and inserts 1.2 seconds of silence across the damaged intervals.
The remaining 1,556 pictures and 2,682 compressed audio frames are preserved,
with the original timing. Missing content is reconstructed, not restored.
The prepared copy is selected automatically once present; the ordinary
importer and strict video/audio checks still apply.

There is no launcher projector to convert: `START` is the entry movie. The
preferences file saves as `LZ9.usr` in the save archive.

Videos are MPEG-1 movies played through Tabuleiro's DirectMedia Xtra. Each
member names its file by the authoring machine's path; the port binds it to
the disc's 8.3 file (`compiler/src/ports.rs`). The Android export transcodes
them with FFmpeg to VP9 and WAV/Opus. The browser importer uses the shared MPEG-1
decoder and produces indexed VP8 with aligned audio. The Mac QuickTime
cast (`videoM`) is pinned as deferred: the runtime answers `the machineType`
as Windows, so it is never used.

Optik's quiz plays its videos through behaviors that swap DirectMedia
members; it has no `getRegOptimal` check.

Checked in the browser runtime's probe: the intro and the Bauwagen hub, without script errors.

Original-projector parity and current physical Android gameplay checks remain
pending.
