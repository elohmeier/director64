# Additional experimental ports

The six formerly catalogued games now have source-pinned port definitions and
registered regression journeys. These are experimental adapters, not a claim
that every game feature works. Conversion gates and journey failures remain
fatal; none has an expected-failure exemption.

| Game | Source Director | Conversion | First journey result |
| --- | --- | --- | --- |
| Findus garden | 6 | Complete | Guardian selection, saves and garden playback pass |
| Fritz & Fertig | 8.5 | Complete | Player discovery reaches unsupported `DirectoryExists` |
| Löwenzahn 2, July 1998 | 6 | Blocked | `BALL16.DXR` frame 75 requires a palette transition |
| Löwenzahn 3 | 6 | Complete | Login reaches unsupported FileXtra external library |
| Löwenzahn 4 | 7 | Complete | Launcher reaches unsupported `DirectoryToList` |
| Löwenzahn 5 | 8 | Blocked | `BAUWAGEN/BLEIB.MPG` has a broken program-stream packet at byte 16090048 |

The April 1998 Löwenzahn 2 pressing remains separately catalogued. Selecting it
for a port command fails with an explicit edition diagnostic. Its movies and
sounds differ from July; the July policy and texture archive are not applied to it.

Build and run a game's journey with:

```sh
uv run --locked director64 web --game findus-garden
uv run --locked director64 journey --game findus-garden
uv run --locked director64 journey --game all --list
```

A failed conversion cannot produce a qualified build. With no build present,
the journey fails at the build check. The checked-in plans for blocked games
record their intended startup route; those routes have not been executed.
The new startup journeys stop at the first unsupported service. Deeper activity
coverage and performance qualification remain work for those ports after their
startup failures are resolved. Garden's journey also verifies guardian selection
and save generations and soaks the garden for one simulated minute.

An unchanged-build Garden repeat exceeded the default 25% timing threshold:
its startup command took about 100 ms in the baseline and 135 ms in the repeat.
The comparison remains failed. This startup variance needs a repeatability
review; a passing gameplay route is not performance qualification.

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
changed inputs fail. This does not repair its separately damaged BLEIB video.

No new original-projector or Android TV validation is claimed. Rendered Wasm
journeys are implementation regressions, and their timing/memory reports measure
the host probe. Failed or incomplete journeys cannot be performance baselines.
