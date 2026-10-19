# Android TV player

`director64 android build --game <slug>` builds one APK for one game: the
engine compiled for Android and the game converted from your own disc. The
APK starts the game directly from the TV's launcher. It was written for an
Epson EF-62N projector (Google TV, Android 14, MediaTek MT9676 with four
Cortex-A55 cores, 2 GiB RAM and a 32-bit `armeabi-v7a` userland) played with
a DualShock 4 or the remote.

**The APK contains the converted game.** It is for your own use: build it
into the ignored `dist/`, install it on your own devices, and never commit,
publish or redistribute it.

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

Sizes are the single-ABI `armeabi-v7a` packages. The emulator observations
came from earlier Android 14 x86_64 builds; they are not gameplay
qualification of the current APKs.

## Status on the projector

- Earlier projector measurements showed Mucklas holding 60 service ticks per
  second with no dropped time at about 15% CPU and 130 MB resident, and
  Christmas's day 5 (Not Transparent ink) playing. Lernerfolg's masked helper
  characters still need projector confirmation.
- Birthday's bedroom scene rendered at 2–3 presentations per second before
  the repair described in [performance-audit.md](performance-audit.md):
  conversion-time source coverage, retained film and mask planes, and
  equivalent-picture detection. A short physical run after the repair
  delivered about 58 Hz service; it ended too early to pass the pacing gate.
  The film-container visibility and cursor fixes that followed
  ([follow-up](performance-audit.md#visibility-and-cursor-follow-up)) are not
  yet checked on the projector.
- All fourteen APKs are built from one clean source commit, with matching
  source and conversion receipts and one signing certificate, and are
  installed on the projector as in-place updates. Each passes a local
  6,000-tick startup smoke without a runtime failure or script alert. Those
  smokes use blank font coverage, nominal audio lifetimes and no video
  decoding; physical gameplay and performance qualification remain pending.
- Löwenzahn 7 and Optik build from separately pinned repaired images
  ([video.md](video.md#preparing-separate-repaired-images)); the reconstructions
  enable the builds but do not restore missing original content.

A successful install and a matching APK hash establish deployment, not a
startup check or a playthrough.

### Pacing measurements

The diagnostics line in the menu shows ticks, redraws and dropped time. The
game thread also logs `DIRECTOR64 ANDROID_PACING` once per active second:
service delivery, redraws, service, render and presentation costs, dropped
time and script alerts. Menu and lifecycle holds reset the window. Capture
the `director64` logcat tag during a scenario, then run:

```sh
uv run --locked python -m director64.android_pacing build/android-pacing.log
```

It fails on incomplete or malformed measurements, script alerts, or three
consecutive windows below 95% of 60 Hz service, and needs ten usable windows
by default. An idle scene can correctly have zero redraws. It cannot prove
visual correctness, input responsiveness or the absence of ANRs; check those
on the device as well.

## Building

Requirements: Node with the locked dependencies, Rust with the
`wasm32-unknown-unknown` target, a host C17 compiler, Docker or a compatible
engine (Emscripten for the converter's tools, and the Android toolchain
image), and FFmpeg with libvpx and libopus (video and long sounds; MPEG-1
decoding for Löwenzahn 6–8, Optik and the Adventskalender).

```sh
docker build --target sdk -t director64-android:local docker/android
uv run --locked director64 android build --game findus-workshop
```

The image pins a JDK, the SDK platform, the build tools and the NDK; the
build runs in it offline and uses no Gradle. Output:
`dist/<game>/<source>/director64-<game>.apk`, for `armeabi-v7a`,
`arm64-v8a` and `x86_64` by default; `--abi` selects some (the EF-62N needs
`armeabi-v7a`). With ImageMagick present, the build draws the launcher icon
and Google TV banner: the game's box art (the ignored, never committed
`games/<slug>/cover.*`) as the icon and beside the title on the banner, or
the title alone.

A build:

1. **Converts the disc** (`android.convert`) into
   `build/<game>/<source>/converted/`, shared with the web app
   ([web.md](web.md)). `platforms/android/export/export.mjs` runs the
   conversion pipeline (`platforms/web/import/pipeline.mjs`, the Rust
   converter compiled to WebAssembly, on a pool of worker threads) under
   Node; see [architecture.md](architecture.md#conversion). Three encodings
   keep the APK small:
   - **Video:** FFmpeg encodes video as VP9 (CRF 30, at most 600 kbit/s) in
     the player's video container. Löwenzahn 1's Cinepak frames go in as the
     converter decodes them, the 8.5 titles' MPEG-1 files whole
     (`platforms/android/export/video.mjs`).
   - **Sound:** a sound of 8 seconds or more becomes Ogg Opus, 96 kbit/s
     stereo or 64 kbit/s mono. Shorter ones stay PCM WAV, so they start at
     once.
   - **Images:** each image is stored as lossless WebP (`D64W`, the vendored
     libwebp) where that is smaller than deflate and the round trip is exact.

   A conversion is reused only when the converter tools, runtime ABI,
   selected source, repaired-image selection, profile and policy and the
   exporter match, and every packed asset has its recorded size and SHA-256.
   `--reconvert` forces a new one. The converter's tools are rebuilt when
   their source fingerprint or any tool hash changes. Every package, fresh or
   reused, must load with the profile-specific C package loader before APK
   assembly.
2. **Compiles the game's runtime** with the NDK: the web runtime's sources
   and flags (`platforms/web/web_runtime.c`, its software compositor, the
   engine, the game's adapter, package mode) without the Emscripten glue,
   plus the Android host (`platforms/android/jni/`), into
   `lib<d64_slug>.so`.
3. **Packages** with aapt2, javac and d8: package
   `org.director64.tv.<slug>` labelled with the game's title, the game under
   `assets/game/` stored uncompressed, native libraries stored and 16 KiB
   aligned. The version code is the minutes since 2026, so every build installs
   as an update and the launcher reloads its cached icon and banner. It signs with a personal key that
   the first build creates at `~/.config/director64/android.keystore`; keep
   it, because an update must be signed with the same key to keep the game's
   saves.

A successful build writes an adjacent `.apk.json` receipt with the source,
ABIs, build-input identity, APK hash and conversion manifest. Starting a
rebuild invalidates it, so a failed build cannot leave an installable stale
APK.

## Installing

`director64 android install` checks the receipt against the current sources
and APK bytes, then runs `adb install -r`. Enable developer options on the
TV (Settings → System → About → press Build seven times) and wireless
debugging, then pair and connect with the addresses its settings screen
shows (the pairing and connection ports differ and can change):

```sh
adb pair <tv-ip>:<pairing-port>
adb connect <tv-ip>:<connection-port>
uv run --locked director64 android install --game findus-workshop --serial <tv-ip>:<connection-port>
```

`adb pair` prompts for the displayed code; `adb mdns services` can discover
the connection address. Updates keep the existing package and signing key
and so preserve saves; uninstalling removes them. An APK copied to a USB
stick can also be opened with a file manager on the TV.

## Playing

| Controller | Game |
| --- | --- |
| Left stick, D-pad, remote D-pad | the cursor (L1 held: fine movement) |
| ✕, A, remote OK | click |
| ○, B | the second button |
| Options, Start | Start |
| Right stick | the direction buttons some games use |
| PS, Share, Back, Menu | the menu |

Controllers map as the web app maps a standard gamepad
(`platforms/web/site/gamepad.js`, ported in `Pads.java`): every device that
gives input takes the lowest free of four player slots, each moves its own
cursor, and the per-game controller layers run unchanged. A DualShock 4
pairs with Google TV directly. Other pads (a ModRetro M64 Pro, an
8BitDo 64) should map their face buttons, Start and stick the same way;
their extra buttons are unverified. The menu's diagnostics show each
device's name and its last key or axes, which is what a mapping needs. A
remote gives its slot back after 30 s idle. A mouse points and clicks; a
keyboard types into games from Director 7 on.

The menu pauses the game and holds the volume, diagnostics, the controls and
quitting. An editable field opens a text dialog with the TV's keyboard, and
the game holds until it is committed. Mucklas takes its player name as key
presses instead: the keyboard opens by itself when the name field starts
taking a name (and again on a click into it), every typed or deleted
character reaches the game's own key handlers at once, OK closes the
keyboard, and the game's confirmation button saves the name (`d64_key_field`
and `d64_type_key` in `platforms/web/web_runtime.c`).

Saves are the game's 128 KiB save image ([architecture.md](architecture.md#runtime-hosts)), written whole
to the app's storage (through a temporary file) after every save and kept
across updates. The TV has no printer: Löwenzahn 1's print requests show a
notice and are dismissed.

## What runs where

| Piece | Responsibility |
| --- | --- |
| `jni/main.c` | JNI bridge and game thread: the 60 Hz service clock and catch-up rule (at most four ticks per pass, twelve ticks of backlog), holds (paused, menu, text entry, print), pads and mouse, text entry, saves, presenting the stage (640×480, or an 800×600 game unscaled) to the surface |
| `jni/packs.c` | Asset packs from the APK's uncompressed assets: indexes parsed by `jni/json.c`, blobs mapped in place, images inflated (D64Z) with zlib; WebP images (D64W) pass through to the runtime's decoder |
| `jni/mixer.c`, `jni/decode.c` | The sound channels through AAudio, with the runtime's loop rule and gains; WAV decoded directly, Ogg Opus sounds and MP3 streams through MediaExtractor and MediaCodec on a decoder thread |
| `jni/video.c` | D5 digital video and D8.5 DirectMedia video through MediaCodec into an image reader, the frame the runtime's time asks for converted for the compositor, its sound in step |
| `java/.../GameActivity.java` | The 4:3 surface, controllers, mouse and keys, the menu and dialogs |
| `java/.../Text.java` | Text boxes set with Android's text renderer, as the web page sets them on a canvas |
| `src/director64/android.py` | Conversion, the make file per game and ABI, packaging and signing |

`mise run test-android-host` checks the keyboard mapping and the Java/JNI
signatures on the host (JDK 11 or newer).

For development without a TV, the image's `emulator` target holds an
Android 14 emulator (`docker build --target emulator -t
director64-android-emulator:local docker/android`; run it with
`--device /dev/kvm`). Its Google APIs image accepts adb's key, which the
Android TV images do not.
