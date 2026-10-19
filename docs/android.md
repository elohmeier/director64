# Android TV player

`director64 android build --game <slug>` builds one APK for one game: the
engine compiled for Android and the game converted from your own disc. The
APK starts the game directly from the TV's launcher. It was written for an
Epson EF-62N projector (Google TV, Android 14, MediaTek MT9676 with four
Cortex-A55 cores and 2 GiB RAM) played with a DualShock 4 or an N64-style
controller.

**The APK contains the converted game.** Like a ROM, it is for your own
use: build it into the ignored `dist/`, install it on your own devices, and
never commit, publish or redistribute it.

| Game | Director | ARM32 APK | Earlier emulator check |
| --- | --- | --- | --- |
| Pettersson und Findus (Workshop) | D6 | 61 MB | intro, farmyard, menu, controller cursor |
| Autos bauen mit Willy Werkel | extended D6 | 64 MB | name entry, save and reload |
| Findus wartet auf Weihnachten | D7 | 55 MB | boots |
| Findus bei den Mucklas | D8 | 192 MB | boots |
| Findus: Der Torten werfende Geburtstagskater | D8.5 | 150 MB | not rechecked |
| Findus: Hühnertanz und Katerzirkus | D8.5 | 117 MB | not rechecked |
| Findus: Die Gespenster-Erschreckmaschine | D8.5 | 101 MB | not rechecked |
| Löwenzahn 1 | D5 | 323 MB | intro video plays |
| Lernerfolg Deutsch 1/2 | D10 | 343 MB | login, MP3 and WAV streams |
| Löwenzahn 6 | D8.5 | 325 MB | login |
| Löwenzahn 7 | D8.5 | 345 MB | login on an earlier build |
| Löwenzahn 8 | D8.5 | 371 MB | login, intro video |
| 25 Jahre Löwenzahn: Optik + Mechanik | D8.5 | 507 MB | Bauwagen on an earlier build |
| Löwenzahn Adventskalender | D8.5 | 165 MB | December countdown |

Sizes describe the current single-ABI `armeabi-v7a` packages. The emulator
observations above came from earlier Android 14 x86_64 builds; they are not
fresh gameplay qualification of these APKs. The EF-62N runs a 32-bit userland.
Earlier projector measurements showed Mucklas holding 60 service ticks per
second with no dropped time at about 15% CPU and 130 MB resident, and Christmas's
day 5 (Not Transparent ink) playing. Lernerfolg's scene covers were checked in
the emulator; its masked helper characters still need projector confirmation.
The diagnostics line in the menu shows ticks, redraws and dropped time.

The worker also logs `DIRECTOR64 ANDROID_PACING` once per active second,
including service delivery, redraws, separate service/render/presentation costs,
dropped time and script alerts. Menu and lifecycle holds reset the measurement
window. Capture the `director64` logcat tag during the scenario, then run:

```sh
uv run --locked python -m director64.android_pacing build/android-pacing.log
```

This exits unsuccessfully for incomplete or malformed measurements, script
alerts, or three consecutive windows below 95% of 60 Hz service. Ten usable
windows are required by default; a longer run and scene-specific checks remain
necessary. An idle scene can correctly have zero redraws. The pacing gate
cannot prove visual correctness, input responsiveness or absence of Android
ANRs; check those during the device run as well. Device performance is a
separate qualification step from compiling or installing an APK.

Strict fresh export rejects Löwenzahn 7's and Optik's original damaged streams.
The separate [media repair workflow](video.md#preparing-separate-repaired-images)
now produces verified derivative images that pass the ordinary importer and
decoders. Original images remain unchanged. Existing installed copies are
retained when a rebuild fails. A successful install and matching APK hash
establish deployment, not a startup check or a complete playthrough.

On 2026-10-18, all twelve buildable ARM32 APKs were installed on the EF-62N
with in-place updates. Their original installation identities were retained;
Löwenzahn 7 and Optik kept their earlier versions. The running foreground app
was left in place, so this deployment did not repeat the gameplay checks above.

A subsequent startup test of that Birthday APK exposed sustained bedroom
rendering at about 2–3 presentations/second and a 10-second input-dispatch
timeout with diagnostics enabled. The matching host replay attributes nearly
all measured work to rendering and reveals many identical redraws. See the
[performance follow-up](performance-audit.md) for measurements, baseline findings
and the preprocessing design; successful installation does not qualify this
scene for practical play.

The subsequent [repair](performance-audit.md#repair-and-regression-evidence)
adds conversion-time source coverage, retained film/mask planes and equivalent
picture detection, together with input, UI, audio and video fixes. The fresh
Birthday replay matches all 300 frames byte-for-byte against the original
renderer while reducing redraws to 28. Host, sanitizer and browser evidence
passes. A later EF-62N reconnection allowed an in-place Birthday update and a
short physical run of repair `be06bbd`.
At that stage, twelve ARM32 APKs had fresh exports and verified current-source
receipts for repair `be06bbd`; Löwenzahn 7 and Optik still had source blockers.
Birthday was the only repair APK installed during that reconnection.

That run delivered 1,039 service ticks in 17.913 active seconds, about 58 Hz.
Animated windows generally reached 11–12 redraws/second, with costly frames
and 0.506 seconds of dropped time. The run ended after an unsupported standard
cursor, leaving only 17 measurement windows; the 60-second qualification gate
correctly failed for insufficient data. It does not qualify performance,
visual correctness or lifecycle behavior.

The subsequent offline investigation found native film-container effects
incorrectly fading the animals and missing XML aliases selecting the wrong
animation state. Original Director 8.5.1 probes establish both behaviors; see
the [follow-up evidence](performance-audit.md#visibility-and-cursor-follow-up).
The follow-up also adds standard tool cursors and a bounded background cache.
Projector interaction stopped at the user's request while it was being used
for a movie. Installation was deferred until authorization on 2026-10-19;
physical gameplay checks remain pending.

All fourteen ARM32 APKs are now built from clean source commit `152f450`, with
matching source/conversion receipts, embedded version names and a common
verified signing certificate. Löwenzahn 7 and Optik have fresh exports from
their separately pinned repaired images; the other twelve verified exports
remain reusable. Each passes a local 6,000-tick startup smoke: 84,000 ticks total
without a runtime failure or script alert. These checks use blank font coverage,
nominal audio lifetimes and no video decoding; they do not qualify full gameplay
or device performance. The repaired streams separately pass strict FFmpeg and
production decoder checks. On 2026-10-19, all fourteen APKs were installed on the
EF-62N as in-place updates. Each installed APK's SHA-256 matches its verified
build; application IDs and original installation times remain unchanged.
No games were launched during this deployment, so physical gameplay and
performance qualification remain pending.

Löwenzahn 7's long clip comes from an independently recovered copy; each of its
five short clips contains one explicit 40 ms picture hold. Optik's affected clip
contains 3.32 seconds of picture holds and 1.2 seconds of silence across the
damaged intervals. Healthy pictures and compressed audio remain intact. These
reconstructions enable the builds but do not restore missing original content.

## Building

Requirements are the browser player's (Node with the locked dev
dependencies, Rust, a host C17 compiler, Docker for the importer's tools) plus FFmpeg with libvpx
(Löwenzahn's video; MPEG-1 decoding for Löwenzahn 6–8, Optik and the
Adventskalender) and the Android toolchain image:

```sh
docker build --target sdk -t director64-android:local docker/android
uv run --locked director64 android build --game findus-workshop
```

The image pins a JDK, the SDK platform and build tools and the NDK; the
build runs in it offline and uses no Gradle. Output:
`dist/<game>/<source>/director64-<game>.apk`, for `armeabi-v7a`,
`arm64-v8a` and `x86_64` (`--abi` selects some; many TV boxes run a 32-bit
userland). ImageMagick, when present, writes the game's title onto the
launcher banner.

A build:

1. **Converts the disc** with the browser importer's pipeline under Node
   (`platforms/android/export/export.mjs`), the same stages the page runs,
   into `build/android/<game>/<source>/game/`. Three encodings keep the
   APK small:
   - **Video:** FFmpeg encodes Löwenzahn's video as VP9 (CRF 30, at most
     600 kbit/s) in the page's video container. Löwenzahn 1's Cinepak frames
     go in as the converter decodes them, the 8.5 titles' MPEG-1 files whole
     (`platforms/android/export/video.mjs`).
   - **Sound:** a sound of 8 seconds or more (the movies' soundtracks, long
     speech) becomes Ogg Opus, 96 kbit/s stereo or 64 kbit/s mono. Shorter
     ones stay PCM WAV, so they start at once.
   - **Images:** each image is stored as lossless WebP (`D64W`,
     `platforms/web/webp_image.h`, the vendored libwebp in
     `third_party/libwebp`) where that is smaller than deflate. The importer's
     `webp` tool keeps one only after its decoder gives back the exact stored
     bytes, and the runtime decodes it when the image loads.

   Löwenzahn 6 went from 1.9 GB to under 0.6 GB.

   A conversion is reused only when the importer, runtime ABI, selected source,
   repaired-image selection, profile/policy and Android exporter match, and every
   packed asset has its recorded size and SHA-256. `--reconvert` forces a new one. Importer tools are
   rebuilt when their source fingerprint or any generated tool hash changes;
   an old browser-site manifest cannot authorize stale conversion tools.
   Both fresh and reused packages must also load successfully with the actual
   profile-specific C package loader. This validates member records, styles,
   palettes and bytecode against the runtime ABI before APK assembly.
2. **Compiles the game's runtime** with the NDK: the browser runtime's sources
   and flags (`platforms/web/web_runtime.c`, its software compositor, the
   engine, the game's adapter, package mode) without its Emscripten glue,
   plus the Android host (`platforms/android/jni/`), into
   `lib<d64_slug>.so`.
3. **Packages** with aapt2, javac and d8: package
   `org.director64.tv.<slug>` labelled with the game's title, the game under
   `assets/game/` stored uncompressed, native libraries stored and 16 KiB
   aligned. It signs with a personal key that the first build creates at
   `~/.config/director64/android.keystore`; keep it, because an update must
   be signed with the same key to keep the game's saves.

## Installing

A successful build writes an adjacent `.apk.json` receipt containing the selected
source, ABIs, build-input identity, APK hash and asset manifest. Starting a rebuild
invalidates the old receipt; a failed build cannot leave an installable stale APK.
The CLI checks that receipt against current sources and APK bytes before ADB runs.

Over the network: enable developer options on the projector (Settings →
System → About → press Build seven times), turn on network debugging, then

```sh
adb connect <projector-ip>:5555
uv run --locked director64 android install --game findus-workshop --serial <projector-ip>:5555
```

For wireless debugging with pairing, use the addresses shown on its settings
screen. The pairing port and connection port differ, and can change:

```sh
adb pair <projector-ip>:<pairing-port>
adb connect <projector-ip>:<connection-port>
adb devices -l
uv run --locked director64 android install --game findus-workshop --serial <projector-ip>:<connection-port>
```

`adb pair` prompts for the displayed pairing code. `adb mdns services` can
discover the connection address. Updates use `adb install -r`; keeping the
existing package and signing key preserves saves. Uninstalling the game
removes its app data.

Or copy the APK to a USB stick and open it with a file manager on the TV,
allowing that app to install unknown apps.

## Playing

| Controller | Game |
| --- | --- |
| Left stick, D-pad, remote D-pad | the cursor (L1 held: fine movement) |
| ✕, A, remote OK | click |
| ○, B | the second button |
| Options, Start | Start |
| Right stick | the C buttons |
| PS, Share, Back, Menu | the menu |

Controllers map as the browser player maps a standard gamepad
(`platforms/web/site/gamepad.js`, ported in `Pads.java`): every device that
gives input takes the lowest free of the console's four ports, each moves its
own cursor, and the per-game controller layers run unchanged. A DualShock 4
pairs with Google TV directly. An N64-style pad (ModRetro M64 Pro,
8BitDo 64) should map its A, B, Start and stick the same way; whether its C
buttons arrive as the right stick depends on the pad and is unverified. The
menu's diagnostics show each device's name and its last key or axes, which
is what a mapping needs. A remote gives its port back after 30 s idle. A
mouse points and clicks; a keyboard types into games from Director 7 on.

The menu pauses the game and holds the volume, diagnostics, the controls and
quitting. An editable field opens a text dialog with the TV's keyboard, and
the game holds until it is committed. Mucklas takes its player name as key
presses instead, as on the console: the keyboard opens by itself when the
name field starts taking a name (and again on a click into it), every typed
or deleted character reaches the game's own key handlers at once, OK closes
the keyboard, and the game's confirmation button saves the name
(`d64_key_field` and `d64_type_key` in `platforms/web/web_runtime.c`). Saves are the console's FlashRAM image,
written whole to the app's storage after every save and kept across updates.
The TV has no printer: Löwenzahn's print pages say so and are dismissed, as
on the console.

## What runs where

| Piece | Responsibility |
| --- | --- |
| `jni/main.c` | JNI bridge and game thread: the page's 60 Hz service clock and catch-up rule, holds (paused, menu, text entry), pads and mouse, text entry, printing, saves, presenting the stage (640×480, or an 800×600 game unscaled) to the surface |
| `jni/packs.c` | Asset packs from the APK's uncompressed assets: indexes parsed by `jni/json.c`, blobs mapped in place, images inflated (D64Z) with zlib; WebP images (D64W) pass through to the runtime's decoder |
| `jni/mixer.c`, `jni/decode.c` | The sound channels through AAudio, with the page's loop rule and gains; WAV decoded directly, Ogg Opus sounds and MP3 streams through MediaExtractor and MediaCodec on a decoder thread, started as far in as they took to decode |
| `jni/video.c` | D5 digital video and D8.5 DirectMedia video through MediaCodec into an image reader, the frame the runtime's time asks for converted for the compositor, its sound in step |
| `java/.../GameActivity.java` | The 4:3 surface, controllers, mouse and keys, the menu and dialogs |
| `java/.../Text.java` | Text boxes set with Android's text renderer, as the page sets them on a canvas |
| `src/director64/android.py` | Conversion, the make file per game and ABI, packaging and signing |

For development, the image's `emulator` target holds an Android 14 emulator
(`docker build --target emulator -t director64-android-emulator:local
docker/android`; run it with `--device /dev/kvm`). Its Google APIs image
accepts adb's key, which the Android TV images do not.
