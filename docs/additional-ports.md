# Additional experimental ports

The six formerly catalogued games now have source-pinned port definitions and
registered regression journeys. These are experimental adapters, not a claim
that every game feature works. Conversion gates and journey failures remain
fatal; none has an expected-failure exemption.

| Game | Source Director | Conversion | Rendered journey |
| --- | --- | --- | --- |
| Findus garden | 6 | Complete | Guardian selection, saves and garden playback pass |
| Fritz & Fertig | 8.5 | Complete | Player registration, story entry, save/reboot and profile resume pass |
| Löwenzahn 2, July 1998 | 6 | Complete with substitute fade | Introduction to the wagon passes |
| Löwenzahn 3 | 6 | Complete | Profile selection, preferences and wagon bedroom pass |
| Löwenzahn 4 | 7 | Complete | Rocket login, profile selection, preferences and wagon exterior |
| Löwenzahn 5 | 8 | Complete with pinned media workaround | Profile selection, story skip and wagon exterior |

The April 1998 Löwenzahn 2 pressing remains separately catalogued. Selecting it
for a port command fails with an explicit edition diagnostic. Its movies and
sounds differ from July; the July policy and texture archive are not applied to it.

Build and run a game's journey with:

```sh
uv run --locked director64 web --game findus-garden
uv run --locked director64 journey --game findus-garden
uv run --locked director64 journey --game all --list
```

All 20 default game editions have completed conversion. The default suite
registers 23 rendered journeys. The six new routes now exercise profile or
navigation state beyond startup; their coverage is listed in
[journeys.md](journeys.md). They remain representative regressions, with
later activities needing additional routes.

Cold startup variance previously tripped Garden's unchanged-build timing gate
(about 100 ms versus 135 ms). Repeated comparisons now use the median of at
least three cold runs, retain the timing range, and take the highest memory
measurement. Every repetition must pass correctness and use the same command
stream. The 25% timing and 10% memory thresholds are unchanged.

## Source and runtime evidence

All six policies pin the selected container hash, Director file count and
recovered resource/script counts. Engine selection comes from recovered movie
headers and projector archives; Fritz's loaded engine DLLs also identify 8.5r321.
Its installation locator is not used as the playable START movie. The exact
Findus/Fritz disc-check handlers have individually hashed compatibility patches.
Löwenzahn 3 retains its startup handler and receives virtual CD/application drive
roots in a separately pinned initializer, replacing the installer's drive scan.

The behavioral/format reference was a clean ScummVM checkout at
[`41ac2b31847622d0662d22c03fe6979e3b43cfbc`](https://github.com/scummvm/scummvm/tree/41ac2b31847622d0662d22c03fe6979e3b43cfbc/engines/director).
The relevant facts concern archive ownership, score extents, palette lookup,
raw sound extents and rich-text backing images. No ScummVM implementation or
stock tile pixels are incorporated. The eight Löwenzahn 2 stock tiles are read
from its original executable, with the owner and byte range independently hashed.
The ProjectorRays version and WASM remain pinned and unchanged; a bounded host
adapter recovers Afterburner SWA sound resources before normal conversion.

The optional `full_engine` profile makes the shared video/live-film services
available to Director 6–8 while retaining their declared versions, numeric
semantics and 24/48-byte source score layouts. Package flag bit 1 selects the
larger member layout; the native loader requires a matching profile. Source-free
writer/loader tests cover all three added layouts. Native tests cover SWA
preload/play/completion/stop, independent audio channels, volume and missing-file
state, plus retained `clickLoc` coordinates.

SWA state values and cue properties follow the
[Macromedia scripting reference](https://eclass.hmu.gr/modules/document/file.php/TP194/drmx2004_scripting_ref.pdf).
Local media corroborate its v3 header, sample delay, cue table and MP3 payload.
Missing linked files retain an explicit error state, rather than invented audio.
Cue ordering, stale palette fallback, retained rich-text rows and old Flash 1
final-frame handling are locally corroborated inferences, not successful original
projector comparisons. Font binding now scopes identically named embedded
subsets to the owning movie/visible casts. Unavailable fonts remain reported.

Löwenzahn 5's truncated `RIECH` sound `Flut_16a` has a complete stereo copy in
`BASTEL/ERFINDER.DCR`. Recovery requires exact source/replacement hashes and an
identical 78-byte playback header. The policy records both resource identities;
changed inputs fail. Its separately damaged BLEIB video uses the explicit
workaround below.

## Best-effort compatibility and source defects

FileXtra now exposes virtual directory existence, creation, listing and file
existence. Fritz keeps each player's files in a scoped virtual folder and its
journey reboots and resumes that saved player. Older basename-only save
profiles retain their existing namespace. Parent traversal is rejected.
Löwenzahn 3–4 receive their preference filenames and virtual CD listings.
The Löwenzahn 4 `ReadPrefs` handler appends separators to its FileIO-open flag;
a SHA-pinned, occurrence-counted patch retargets those two appends to `path`.
Installation-locator and optical-disc serial workarounds are scoped in each
port definition.

Director field expressions evaluate their text, while assignment/property
owners remain field references. This fixes serialized player/pet names.
Text fields honor size and alignment changes; unavailable face changes and
fixed/adjust text boxes retain the native layout and emit diagnostic traces.
Sprite `tell` uses the film's authored `lastChannel`; releasing untouched
channels does not allocate puppet overrides. Negative ordinary sprite numbers
cannot become encoded film children. Explicit Flash property calls against
absent optional source clips return zero and trace the absent target.
Resetting an unused alert hook to zero is accepted during D8 cleanup.

The Löwenzahn 2 palette fade is a truecolor scene-snapshot substitute through
black or white, with speed-derived approximate timing. It is recorded under
`palette_fade_substitutes` and traced as `PALETTE_FADE_TRUECOLOR_SUBSTITUTE`.
Indexed-color interpolation and original timing have not been verified.
Ordinary D6 score tempos of 17/25 FPS now remain distinct from wait opcodes.

Stream packs include the recovered speech/music folders for Löwenzahn 2 and
6–8, Advent and Optik. Exact movie folders determine relative stream paths;
a same-folder primary-volume 8.3 fallback resolves legacy truncated filenames.
Advent explicitly maps the authored `SCHREIBTISCH` audio folder to the disc's
`SCHREIBT` folder; unrelated folders are never searched as substitutes.
Missing packaged audio fails explicitly. Duplicate siblings choose WAV in
Löwenzahn 5 and WAV/AIFF in Löwenzahn 7, recorded as a lossless preference;
this is a format-selection substitute. Advent's unreferenced, headerless
`UNKLAR/NIKOLAUS.WAV` is excluded rather than guessed as an audio format.
QuickTime audio edit rates are resampled onto the presentation timeline
(Löwenzahn 1 uses 65562/65536). Embedded PNG dimensions replace stale cast
bounds, fixing Löwenzahn 8's large dog animation metadata.

Two narrowly pinned source-script patches preserve navigation despite source
defects: Löwenzahn 7 free play skips indexing an empty task playlist on key
release; Löwenzahn 6 skips a narration request for `@:snd:s03i01`, absent from
its recovered disc and casts. The latter stops that channel and emits
`SOURCE_AUDIO_UNAVAILABLE`; no audio is invented.

Löwenzahn 5's damaged `BAUWAGEN/BLEIB.MPG` has SHA-256
`03fc37a3087c45d803d3e79a60237513487a5c586d5ae136d1d2691b2ff4764e`.
Only that exact 31,060,804-byte file uses the audited half-open packet span
[16090048, 16092360), replaced by a same-length padding packet in a scratch
copy. FFmpeg concealment is enabled only for this explicit recipe, and the
conversion receipt retains decoder diagnostics, frame count and both hashes.
The converted video is strictly decoded again and must retain dimensions,
frame rate and all 4,456 frames. This discards damaged data; it is not a
restoration or a claim of original pixels. Other media still use strict
conversion. Original retail media is never modified.

The additional ScummVM facts consulted at the revision above concern FileXtra
return codes/listing separators, field value versus reference evaluation,
score tempos and palette fade flags. Local script/score data and focused
source-free runtime/converter tests corroborate the implementation choices.

No new original-projector or Android TV validation is claimed. Rendered Wasm
journeys are implementation regressions, and their timing/memory reports measure
the host probe. Failed or incomplete journeys cannot be performance baselines.
