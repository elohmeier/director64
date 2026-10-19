# Android TV player

`director64 android build --game <slug>` builds one APK for one game: the
engine compiled for Android and the game converted from your own disc. It
starts the game directly from the TV's launcher. **The APK contains the
converted game**: install it on your own devices only, never publish it.

## Building

Requirements: what the conversion needs (Rust, a host C/C++ compiler, Node
with the locked dependencies, FFmpeg with libvpx and libopus) and Docker or
a compatible engine for the Android toolchain image, which pins a JDK, the
SDK platform, build tools and the NDK (no Gradle; the build runs offline).

```sh
docker build --target sdk -t director64-android:local docker/android
uv run --locked director64 android build --game findus-workshop
```

| Option | Effect |
| --- | --- |
| `--abi ABI` | Only these ABIs (repeatable): `armeabi-v7a`, `arm64-v8a`, `x86_64`; default all |
| `--reconvert` | Convert the disc image again |
| `--jobs N` | Parallel native compile jobs |

A build converts the disc into `build/<game>/<source>/converted/` (shared
with the web app, see [architecture.md](architecture.md#conversion)),
compiles the runtime with the NDK (the web runtime's sources and flags plus
`platforms/android/jni/`), and packages it with aapt2, javac and d8 as
`org.director64.tv.<slug>`. The game ships uncompressed under
`assets/game/`. Output: `dist/<game>/<source>/director64-<game>.apk` and an
`.apk.json` receipt (source, ABIs, input digest, APK hash).

The version code is the minutes since 2026, so every build installs as an
update and the TV launcher refreshes its cached icon and banner. With
ImageMagick present, the build draws the icon and Google TV banner from
`games/<slug>/cover.*` (local box art, never committed) or from the title
alone.

The APK is signed with a personal key the first build creates at
`~/.config/director64/android.keystore`. Keep it: updates must be signed with
the same key to keep the game's saves.

## Installing

`director64 android install` checks the receipt against the current sources
and APK, then runs `adb install -r`. Enable developer options on the TV
(Settings → System → About → press Build seven times) and wireless
debugging, then:

```sh
adb pair <tv-ip>:<pairing-port>
adb connect <tv-ip>:<connection-port>
uv run --locked director64 android install --game findus-workshop --serial <tv-ip>:<connection-port>
```

The connection port changes when wireless debugging restarts. Updates keep
saves; uninstalling deletes them. Devices with a 32-bit userland need
`armeabi-v7a`.

## Playing

| Controller | Game |
| --- | --- |
| Left stick, D-pad, remote D-pad | Cursor (L1 held: fine movement) |
| ✕, A, remote OK | Click |
| ○, B | Second button |
| Options, Start | Start |
| Right stick | Direction keys some games use |
| PS, Share, Back, Menu | Menu |

Every device that gives input takes the lowest free of four player slots
and moves its own cursor (`Pads.java`, the same mapping as the web app's
`gamepad.js`). A DualShock 4 pairs with Google TV directly; other pads
should map face buttons, Start and stick the same way. The menu's
diagnostics show each device's name and last key or axes, which is what a
new mapping needs. A mouse points and clicks; a keyboard types from
Director 7 on.

The menu pauses the game and holds volume, diagnostics, controls and quit.
An editable field opens a text dialog with the TV keyboard; a game with a
`key_field` (or Mucklas' name entry) receives typed keys directly instead.
Saves are written whole to app storage after every commit. The TV has no
printer: print requests show a notice.

## Pacing

The game thread logs `DIRECTOR64 ANDROID_PACING` once per active second
(service delivery, redraws, costs, dropped time, script alerts). Capture the
`director64` logcat tag during a scenario and check it with:

```sh
uv run --locked python -m director64.android_pacing pacing.log
```

It fails on malformed windows, script alerts, or three consecutive windows
below 95% of 60 Hz service.

## Source layout

| Piece | Responsibility |
| --- | --- |
| `jni/main.c` | JNI bridge and game thread: clock, holds, pads and mouse, text entry, saves, presentation |
| `jni/packs.c`, `jni/json.c` | Asset packs mapped from the APK |
| `jni/mixer.c`, `jni/decode.c` | Sound through AAudio; Opus and MP3 via MediaCodec |
| `jni/video.c` | Video via MediaCodec |
| `java/.../GameActivity.java`, `Text.java` | Surface, controllers, menu, dialogs, text rendering |
| `src/director64/android.py` | Conversion, make file per ABI, packaging and signing |

`mise run test-android-host` checks the keyboard mapping and JNI
signatures on the host. Without a TV, the image's `emulator` target runs an
Android 14 emulator (`docker build --target emulator -t
director64-android-emulator:local docker/android`, run with
`--device /dev/kvm`); TV system images refuse adb's key, so it uses Google
APIs.
