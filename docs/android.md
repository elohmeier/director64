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
| `--validation` | Build a separate test-only APK with read-only runtime observations |

A build converts the disc into `build/<game>/<source>/converted/` (shared
with the web app, see [architecture.md](architecture.md#conversion)),
compiles the runtime with the NDK (the web runtime's sources and flags plus
`platforms/android/jni/`), and packages it with aapt2, javac and d8 as
`org.director64.tv.<slug>`. Under `assets/game/`, encoded media blobs stay
uncompressed for mapping, while the package and JSON indexes use APK
compression. Output: `dist/<game>/<source>/director64-<game>.apk` and an
`.apk.json` receipt (source, ABIs, input digest, APK hash, pack size savings).
Builds hold a per-edition conversion lock until their asset snapshot is complete;
a concurrent web build or reconversion waits rather than replacing files that
the APK builder is still reading.

`mise run build-android-all` builds every local port for all three ABIs and
verifies each native ELF plus its current source receipt. A final pass checks
all receipts again after the batch. Per-game logs and the static qualification
summary remain under `build/validation/android-builds/`; this command does not
install apps or contact a device.

Android staging compacts the shared conversion without re-encoding media.
Each `<kind>.json` is `{version: 1, blob: "<blob>", entries: {name:
[offset, length, ...]}}`; offsets address `<blob>.bin`. Names with identical
payloads share a range. Sound members and `playFile` streams both address
`audio.bin`, with stream durations retained in their own index. There is no
separate `streams.bin` in the APK. Ogg Opus comparisons ignore only stream
serial numbers and their dependent page checksums, retaining packet bytes,
source tags, flags and granules. The first complete original encoded file
is stored unchanged. Other formats compare byte for byte. Web conversion
files retain their existing layout.

The version code is the minutes since 2026, so every build installs as an
update and the TV launcher refreshes its cached icon and banner. With
ImageMagick present, the build draws the icon and Google TV banner from
`games/<slug>/cover.*` (local box art, never committed) or from the title
alone.

Games can set `title = "Short title"` in their `game.toml`'s `[android]` table for the
TV launcher, banner and player menu. This keeps the activity name visible
on narrow TV tiles while the catalog retains the full title. Package names
and saves are independent of this label.

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

Controllers, the remote, mouse and touchpad share one game pointer. A new
controller press or deliberate stick movement takes control; inactive devices'
releases cannot clear another device's held buttons. There are no player slots.

Physical mice and touchpads use Android's smooth native arrow, text, crosshair
and wait icons when the game's resolved cursor has that standard role. Authored
bitmap, masking and inverting cursors retain their software artwork while the
stage hides Android's cursor with `PointerIcon.TYPE_NULL`. Controller navigation
also uses one software cursor. The native/software handoff acknowledges hiding
the Android icon before drawing the software fallback, and removes software
pixels before enabling a native icon. Menus, text entry, document pickers and
printing hide the stage cursor while preserving their normal Android pointers.
Actual PS4 touchpad/projector behavior still requires physical validation.

The bounded Android 14 emulator proof uses an authored source-free fixture and
standard Android mouse hover events injected through the shell input service.
It verifies native arrow/text/wait roles, cursor hiding, an authored inverting
software fallback, controller/mouse handoff and the real document picker. A
separate check starts with a displayed software cursor, opens the native menu,
requires the canvas cursor to disappear while the engine clock is held, then
checks that dismissal restores one cursor. Host regressions also inject failed
geometry/lock/post operations and surface replacement: a native icon is enabled
only after a successful current-surface frame post, with cached frames retried
and stale UI acknowledgements rejected. These checks qualify emulator event
routing and presentation policy; physical PS4 input and display pacing remain
unqualified.

```sh
mise run android-emulator start
uv run --locked python tools/android/pointer_fixture.py
uv run --locked python tools/android/pointer_emulator.py \
  --output build/validation/android-pointer-proof --timeout 180
mise run android-emulator stop
```

The menu pauses the game and holds volume, diagnostics, controls and quit.
An editable field opens a text dialog with the TV keyboard; a game with a
`key_field` (or Mucklas' name entry) receives typed keys directly instead.
Saves are written whole to app storage after every commit. Print requests open
Android's print interface, including Save as PDF when available.

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
signatures on the host. For development without a TV, build the emulator image
and start the isolated Android 14 instance (Linux with `/dev/kvm` access):

```sh
docker build --target emulator -t director64-android-emulator:local docker/android
mise run android-emulator start
mise run android-emulator adb devices
mise run android-emulator adb shell getprop sys.boot_completed
mise run android-emulator logs --tail 30
mise run android-emulator stop
```

The task uses the Google APIs x86_64 image with a TV display profile; retail TV
system images refuse the development ADB key. Build with `--abi x86_64` for this
emulator. The repository is mounted as `/workdir`, so the task's `adb install`
accepts repository-relative APK paths. ADB stays inside the offline container;
it does not change the host ADB server or connect to real devices. Each start
after a stop creates fresh state under ignored `build/android/emulator.*`, so
crash leftovers and stale AVD locks do not prevent the next boot. Starting an
already running instance waits for its boot instead of creating another one.
`DOCKER` selects the container engine; `DIRECTOR64_EMULATOR_IMAGE` and
`DIRECTOR64_EMULATOR_CONTAINER` override the image and container names.

The launcher selects `-gpu swangle`: ANGLE over Vulkan SwiftShader. Emulator
37.2.12's legacy GLES SwiftShader puts generated shader code on the heap and
continues after `mprotect(PROT_EXEC)` fails on hosts that reject executable heap
pages, causing a startup SIGSEGV. A core dump and a heap-versus-mapped-page
execution probe reproduced this failure; the ANGLE backend boots with the same
host policy. Keep Vulkan enabled to retain that backend. The virtual modem is
disabled because its `::1` connection fails in an offline container. Emulator
checks are distinct from validation on the Android TV.

## Automated Android journeys

Build the validation variant to replay gameplay through Android rather than
the host probe. It uses the separate package
`org.director64.tv.<slug>.validation`, its own save data and preferences, and
an APK named `director64-<slug>-validation.apk`. Its manifest is debuggable and
test-only, and its launcher label starts with `Test:`. Production APKs expose no
observation receiver. The native game
thread publishes read-only state snapshots. The runner sends pointer events
and text through Android's input dispatch and EditText dialog; it cannot
execute a gameplay RPC or assign runtime state.

With the isolated emulator running:

```sh
uv run --locked director64 android build --game findus-birthday --abi x86_64 --validation
uv run --locked director64 android-validate --game findus-birthday --emulator \
  --exercise-lifecycle --output build/android-validation/birthday
```

The runner verifies the current APK receipt, installs with `adb install -r -t`,
and clears only the validation package before each journey. Save-preserving
reboots within a journey retain that session's data. It verifies the installed
build identifier as well, including when `--no-install` is used. Existing output
directories are refused so a later run cannot silently overwrite evidence.

The registered `steps` journeys use their existing stage coordinates and state
expectations. A legacy Python journey needs a recorded `replay.json` passed with
`--replay`; raw analog pad samples and standalone probe key commands are refused
because ADB cannot reproduce their exact timing. A custom `--plan` can include
`android_key` actions with `source` (`keyboard`, `dpad`, or `gamepad`), an Android
`KEYCODE_*`, and optional `longpress`, or `lifecycle` actions (`menu`, `home`,
`resume`, `reboot`). ADB source selection exercises Android dispatch, not the
mapping of a physical controller. Text replay supports ASCII name characters;
other keyboard layouts require a physical-input scenario.

`--exercise-lifecycle` appends menu/dismiss and Home/resume checks, including
whether held game time advances. `--expect-animation` requires motion in a final
five-frame sequence; `--expect-audio` requires non-silent output during a final
interval. Enable these output assertions only for a scene expected to animate
or sound. Custom plans can specify bounded `sequence` observations and `audio`
rules (`min_rms`, `max_rms`, `min_seconds`, `min_nonzero_samples`,
`max_clipped_ratio`). Audio counters measure the actual post-mix AAudio callback
signal, including the native decoder and mixer. They do not record speaker or
HDMI sound, and the report states that acoustic output was not captured.

Every run retains the input journal, observations, full-screen and normalized
stage PNGs, sequence samples, process-filtered logcat and pacing reports. Crashes,
script alerts, missing observations, failed state/output assertions and pacing
failures produce a failing report. `--timeout` bounds the full journey. A run
without `--baseline` records visual evidence as unbaselined. Reviewed baselines
use the shared [visual comparison workflow](validation.md); `--baseline` then
adds regional image comparisons and difference images. All game-dependent
evidence remains under ignored `build/`.
During Home/background checks the screenshot shows Android system UI; these
checkpoints are marked `system-ui`, and their normalized image is the former
stage viewport rather than a claim that the game surface remained visible.

For a later physical-device run, select its already connected ADB serial
explicitly; the runner never discovers or connects a projector by itself:

```sh
uv run --locked director64 android build --game findus-birthday --abi armeabi-v7a --validation
uv run --locked director64 android-validate --game findus-birthday --serial <connected-serial> \
  --exercise-lifecycle --output build/android-validation/physical-birthday
```

Reports identify the device fingerprint, ABI list, emulator status and exact
APK/source/plan identity. Emulator and physical-device baselines remain separate.
Passing an automated route does not establish every activity, controller mapping
or acoustic output; original-projector comparison and broad hardware
qualification remain separate claims.

The document bridge uses Android's document picker for FileIO open/save,
preserves selected URI capabilities privately, and uses PrintManager for
authored PrintOMatic pages. Löwenzahn 1 prints the recovered chapter text and
artwork from its packaged print documents. Game time and audio pause during
these system dialogs. Images retain their original bytes; decoded images are
limited to 4096 pixels per side and 32 MiB of RGBA, with a 16 MiB input limit.
Oversized or undecodable choices display an error and permit retry/cancel.
Selected names and runtime text use UTF-8. CAR input accepts UTF-8 or legacy
Windows-1252 text; export encodes strictly as Windows-1252 for the original
desktop format and reports characters that format cannot represent. Raster
image bytes remain exact even when a selected file has a misleading extension.

A source-free authored fixture exercises the production compiler, interpreter,
JNI queue, Android document picker and PrintManager without retail media:

```sh
uv run --locked python tools/android/user_files_fixture.py --jobs 4
mise run android-emulator start
uv run --locked python tools/android/user_files_emulator.py \
  --output build/android-documents-check
```

The emulator tool requires Poppler's `pdfinfo` and `pdftoppm`. It verifies exact
CAR bytes, image colors, an actual one-page A4 PDF, modal cancellation and
process restart. Its dedicated `.validation` app and Download folder keep the
player's app data separate. Recordings and APK receipts stay under `build/`.
Bitmap-cast activities in Löwenzahn 6, 7 and Advent have 16 MiB save generations;
other games retain the 64 KiB default. Valid legacy preference saves migrate
without changing their file bytes; failed reads never replace the saved image.
