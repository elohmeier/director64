# Android TV player

`director64 android build --game <slug>` builds one APK for one game: the
engine compiled for Android and the game converted from your own disc. The
APK starts the game directly from the TV's launcher. It was written for an
Epson EF-62N projector (Google TV, Android 14, MediaTek MT9676 with four
Cortex-A55 cores and 2 GiB RAM) played with a DualShock 4 or an N64-style
controller.

**The APK contains the converted game.** Like a ROM, it is for your own
use: build it into the ignored `dist/`, install it on your own devices, and
never commit, upload or share it.

| Game | Director | APK | Checked in the emulator |
| --- | --- | --- | --- |
| Pettersson und Findus (Workshop) | D6 | 95 MB | plays: intro, farmyard, menu, controller cursor |
| Autos bauen mit Willy Werkel | extended D6 | 85 MB | plays: name entry, save and reload |
| Findus wartet auf Weihnachten | D7 | 92 MB | boots |
| Findus bei den Mucklas | D8 | 297 MB | boots |
| Löwenzahn 1 | D5 | 524 MB | boots, intro video plays |
| Lernerfolg Deutsch 1/2 | D10 | 486 MB | boots to the login, MP3 and WAV streams play |

These checks ran on an Android 14 emulator (x86_64). On the EF-62N all six
install, and Mucklas, Christmas and Lernerfolg run; its userland is 32-bit
only (`armeabi-v7a`). Mucklas held 60 ticks per second there with no dropped
time at about 15% CPU and 130 MB resident, and Christmas's day 5 (Not
Transparent ink) plays there.
Lernerfolg's scene covers were checked in the emulator; its masked helper
characters are not yet confirmed on the projector. Scene entry on the other
games is unmeasured; the diagnostics line in the menu shows ticks per
second, redraws and dropped time.

## Building

Requirements are the browser player's (Node with the locked dev
dependencies, Rust, Docker for the importer's tools) plus FFmpeg with libvpx
(Löwenzahn's video) and the Android toolchain image:

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
   into `build/android/<game>/<source>/game/`. Sounds stay PCM WAV; FFmpeg
   encodes Löwenzahn's video as VP8 in the page's video container. A
   conversion by the same importer revision and runtime ABI is reused;
   `--reconvert` forces a new one. The importer's tools are built first when
   `build/web/tools` is missing.
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

Over the network: enable developer options on the projector (Settings →
System → About → press Build seven times), turn on network debugging, then

```sh
adb connect <projector-ip>:5555
uv run --locked director64 android install --game findus-workshop --serial <projector-ip>:5555
```

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
| `jni/main.c` | JNI bridge and game thread: the page's 60 Hz service clock and catch-up rule, holds (paused, menu, text entry), pads and mouse, text entry, printing, saves, presenting the 640×480 stage to the surface |
| `jni/packs.c` | Asset packs from the APK's uncompressed assets: indexes parsed by `jni/json.c`, blobs mapped in place, images inflated (D64Z) with zlib |
| `jni/mixer.c`, `jni/decode.c` | The sound channels through AAudio, with the page's loop rule and gains; WAV decoded directly, MP3 streams through MediaCodec on a decoder thread, started as late as they decoded |
| `jni/video.c` | D5 digital video through MediaCodec into an image reader, the frame the runtime's time asks for converted for the compositor, its sound in step |
| `java/.../GameActivity.java` | The 4:3 surface, controllers, mouse and keys, the menu and dialogs |
| `java/.../Text.java` | Text boxes set with Android's text renderer, as the page sets them on a canvas |
| `src/director64/android.py` | Conversion, the make file per game and ABI, packaging and signing |

For development, the image's `emulator` target holds an Android 14 emulator
(`docker build --target emulator -t director64-android-emulator:local
docker/android`; run it with `--device /dev/kvm`). Its Google APIs image
accepts adb's key, which the Android TV images do not.
