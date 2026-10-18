# Digital video

The QuickTime Xtra and DirectMedia Xtra use the same runtime video service.
The converter resolves authored directories and filenames against the selected
disc's exact paths and verified aliases. It preserves the source Xtra identity,
duration, pause/loop/sound flags, and dimensions when it binds a converted video.
An unresolved source, absent decoder, missing output asset, or invalid duration
is a conversion or playback error.

The current MPEG path accepts MPEG-1 video in MPEG-1 or MPEG-2 program streams.
The shared Rust parser separates the elementary streams and checks packet
bounds, picture counts, audio frame synchronization, and timing. MP2 bitrate
and channel-mode changes at frame boundaries are supported; those changes are
not evidence of corrupt media. A sample-rate change remains unsupported.
Streams without audio are valid and produce no audio asset.

QuickTime movies may instead contain sound without a picture track. The source
parser records `audioOnly` for these inputs; model binding retains
`videoAudioOnly`, converted audio and the movie duration, with an empty picture
asset. A missing picture is accepted only with that explicit source fact and a
valid audio output. Missing conversion output still fails. All hosts use the
ordinary independent media voice and clock without opening a picture decoder.

The recovered D5 source has eight audio-only movies among 66 MOV files. A fresh
source parse confirms `HOL3.MOV` contains one raw 8-bit mono audio track at
22,050 Hz and lasts 20.37 seconds (12,222 internal movie ticks). All 66 source
hashes were checked before rebinding cached converted-media records into a fresh
D5 model; package version 9 then passed. This verifies source classification,
metadata binding and packaging, not a new transcode or original-player
comparison. Source-free runtime and browser tests cover sound-only host
requests, pause, seek, resume, exact end, stop time and voice cleanup.

## Platform paths

| Target | Import and playback |
| --- | --- |
| Browser | The pinned MIT PL_MPEG decoder reads demultiplexed MPEG-1 and MP2; WebCodecs encodes VP8 and decodes the resulting indexed pack. |
| Android | FFmpeg re-encodes clips to VP9 and sound to WAV/Opus; MediaCodec decodes the video. Each active video has an independent audio voice. |
| N64 | The pinned SDK converts MPEG to seekable H.264. The shared D5/D8.5/D10 host opens the H.264 stream and aligned wav64 audio. |
| Rendered Node probe | FFmpeg decodes the requested GOP from VP8/VP9 IVF or native Annex B H.264 packs and returns actual RGBA pixels. Unknown codecs and decoder failures stop the probe. |

The N64 MPEG conversion preserves authored geometry and frame rate. It checks
the encoded frame count and rejects silent rescaling or dropped frames. This
is decoder and conversion coverage, not a claim that every source resolution
and frame rate meets real N64 memory or throughput limits. N64 hardware playback
still needs separate qualification.

An indexed pack contains codec, width, height, and timestamp/key/offset/length
records. The parser rejects invalid ranges and a stream without an initial
keyframe. Native H.264 packs may store zero width and height: their SPS supplies
the actual dimensions. Decoded images are bounded to 4096 pixels per axis.

## Timing and instances

The runtime uses 600 ticks per second internally. QuickTime's movie-time
interface uses 60 ticks per second; DirectMedia's current-time interface uses
milliseconds. The decoder consumes the runtime's explicit position, rate, and
seek serial rather than running a separate score clock.

An MPEG open GOP may contain initial B pictures without their preceding
reference pictures. The importer counts those coded pictures, skips them for
presentation, and starts the first displayable picture at timestamp zero. It
verifies both coded and presented frame counts. No missing frame is padded or
duplicated to satisfy a count check.

Android also assigns timestamps from decoded picture order at the sequence
rate. Missing or repeated PES timestamps must not cause duplicated pictures
or non-monotonic encoder output. It checks geometry, rate and frame count
against the shared metadata and validates the resulting IVF index.

Audio and video PTS are compared after that initial-picture adjustment. Early
audio is trimmed; late audio is delayed. Video duration comes from the video
stream rather than the length of an accompanying sound.

Linked movies receive monotonically assigned instance identities. A parent
and two children using score channel 1 therefore have three separate players,
seek positions, and audio voices. Removing an instance closes its players;
allocator address reuse cannot revive stale frames. Child pause and sound flags
also reach the host. Android's eight-player and N64's configured player limits
fail explicitly when exceeded.

## Damage and validation

PL_MPEG is pinned at `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023`.
Local changes are listed in [README.director64](../third_party/pl_mpeg/README.director64).
Video slices are bounded at their next start code, half-pel motion reads include
their neighboring pixels in bounds checks, and malformed slices increment a
public corruption counter. Import rejects a nonzero counter. Initializing frame
storage and restoring parser synchronization contain a failure; they do not
qualify substituted pixels as a repair. Native MPEG conversion also runs a
strict FFmpeg decode before transcoding; APK export uses the same strict
error policy. Encoder process failures and source-iterator errors reject the
export and release the subprocess instead of hanging on a broken input pipe.

The recovered 159-stream corpus contains MPEG-1 video throughout, with 156 MP2
audio streams and three silent streams. The strict browser conversion scan
passed 152 streams and rejected seven source files:

- Five `VIDEO1` through `VIDEO5` files contain byte-identical video. Their final
  B picture has a malformed last slice: the rightmost macroblock requests a
  half-pel neighbor outside the picture. Both the bounded decoder and strict
  FFmpeg decode detect it. This is separate from their legal MP2 mode changes.
- `VI_05_01` has damaged video slices and an audio frame synchronization failure.
- `VLUZIFER` has invalid packet synchronization in its program stream.

These seven inputs remain explicit source-data failures. Producing concealed
output would require a separately specified and validated recovery policy.

### Recovery investigation

An independent investigation on 2026-10-18 compared the failing extracted files
with their pinned ISO extents, inspected MPEG packets and elementary streams,
and checked the hybrid discs' Mac file catalogs. The damaged bytes are present
in the source images; repeating extraction or relaxing the parser cannot
restore them. The Mac data forks occupy the same extents as the Windows files,
and their empty resource forks provide no alternate copy.

| Input | Confirmed defect | Recovery implication |
| --- | --- | --- |
| LZ7 `VIDEO1`–`VIDEO5` | The identical video streams contain one malformed non-reference B picture, display frame 826 at 33.040 seconds, lasting 40 ms. | A bounded one-picture reconstruction is possible without changing other pictures, audio or timing. It is not recovered original artwork. |
| LZ7 `VI_05_01` | 1,583 sectors across 29 runs contain byte-identical material from `VI_04_02`: 3,241,984 overwritten bytes. Their clocks jump ahead by 87.946667 seconds. | Sorting packets cannot replace the missing sectors; it creates duplicate clocks. MP2 CRC protection is disabled in this source. A clean copy is needed for exact restoration. |
| Optik `VLUZIFER` | A valid-checksum HFS+ journal header and eight valid transaction records overwrite at least 158,208 bytes of MPEG data. | This is foreign filesystem data, not MPEG padding. The separate local 7z archive expands to the same damaged ISO. Exact restoration needs another copy. |

The journal identification follows Apple's
[HFS Plus Volume Format](https://developer.apple.com/library/archive/technotes/tn/tn1150.html).
It establishes the type of the overwriting data, not when or how it entered the
image. The containing hybrid disc itself uses classic HFS. Hash and slice-anchor
searches across 271 local MPEG files, including the other Löwenzahn and Lexikon
discs, found no clean compressed duplicate of either affected clip.

Intact LZ7 timestamps establish 3,535 original coded pictures, including two
ordinary initial open-GOP B pictures that cannot be presented. The playable
timeline is therefore 3,533 pictures, or 141.32 seconds. Foreign footage inflates
the damaged stream's picture count, so counting its remaining picture headers
cannot establish the original duration. Source-byte and reference-chain checks
identify 624 untrusted original picture slots (24.96 seconds), separately from
the initial B pictures. Optik's intact endpoints establish a 65.56-second video
timeline. Its 34 missing video slots and 65 missing MP2 slots understate the
damage because additional pictures are concealed or depend on bad references.

A separate local proof reconstructs each short clip's damaged B picture by
holding its preceding P picture for 40 ms. Both the production PL_MPEG adapter
and independent FFmpeg frame hashes establish that the other 827 displayed
pictures are unchanged. All packet metadata, timestamps and elementary audio
bytes are retained; the replacement matches its preceding picture exactly.
Strict FFmpeg video/audio decoding and browser importing accept the result.
All five program-stream copies independently pass strict video/audio decoding,
frame-hash comparison and packet/audio preservation checks.
This qualifies a bounded reconstruction, not recovery of the original picture.

Recovery should preserve the original images and distinguish two outcomes:

- Exact recovery uses another verified read or copy. Re-reading physical media
  with a persistent error map and retries, as documented by
  [GNU ddrescue](https://www.gnu.org/software/ddrescue/manual/ddrescue_manual.html#Optical-media),
  can establish which sectors are readable; the new source must then pass the
  same packet, audio, picture-count and strict decoder checks.
- Reconstruction creates a separate, explicitly identified derivative. Its
  receipt must bind the original hash, recipe, changed regions and output hash.
  It must preserve the timeline and distinguish replaced pictures or sound from
  recovered original data. Changes to a reference picture must account for
  dependent P/B pictures until an independently intact reference is reached.
  Expected damage must never authorize unrelated decoder errors.

These findings do not change the default import gate or qualify earlier APKs
that concealed the errors. No reconstructed stream has been integrated into a
game package or deployed to a device.

A subsequent public-archive search recovered an intact `VI_05_01` from a raw
2352-byte-sector MDF/MDS image. Its 21,731,332-byte file differs from the selected
ISO in exactly the 1,583 previously identified foreign sectors; every other
sector is identical. The recovered clip's SHA-256 is
`5513bc95319ec238310f694f8ad52117814f66503fe76e12f6ef44cd2bde1ead`.
Strict FFmpeg video/audio decoding and the production browser importer pass:
3,533 displayed pictures, 5,412 MP2 frames and 141.32 seconds after the ordinary
initial B-picture adjustment. This is replacement from an independent copy,
without synthesized frames or sound. Only bounded source-image ranges were
retrieved, so this does not qualify the entire alternate disc image.

The same raw image's `VIDEO1` is byte-identical to the malformed selected copy;
the five short clips still need their separately identified 40 ms reconstruction
or another clean source. Two ordinary LZ7 ISO uploads match the selected damaged
ISO's size, MD5 and SHA-1. Both an Optik ISO upload and a separate 7z upload also
match the local damaged files' respective sizes and hashes. They provide no
replacement bytes for Optik. The recovered LZ7 clip remains separate from the
original images and has not yet been integrated into a game package.

Source-free tests cover bounded damaged slices, immutable decoder input,
half-pel bounds, host identity and lifecycle, audio flags, decoder errors, and
generated VP8/H.264 frames across keyframe seeks and rewinds. Native and
sanitizer tests, Android cross-builds, and N64 cross-compilation are distinct
from original-projector comparisons and hardware playback.
