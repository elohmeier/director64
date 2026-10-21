#!/bin/sh
# Runs inside the emulator image; the host task gives each run fresh state.
set -eu

mkdir -p "$ANDROID_USER_HOME" "$ANDROID_AVD_HOME"
echo no | "$ANDROID_HOME/cmdline-tools/latest/bin/avdmanager" create avd \
  -n director64 -k 'system-images;android-34;google_apis;x86_64' -d tv_1080p --force
"$ANDROID_HOME/platform-tools/adb" start-server

# Legacy GLES SwiftShader allocates JIT code on the heap and ignores a denied
# mprotect(PROT_EXEC). ANGLE's Vulkan SwiftShader backend uses mapped JIT pages.
# Keep Vulkan enabled: disabling it can fall back to the legacy GLES driver.
# The phone system image's modem also fails resolving ::1 in an offline container.
exec "$ANDROID_HOME/emulator/emulator" -avd director64 \
  -no-window -no-audio -no-snapshot -no-boot-anim -no-metrics \
  -gpu swangle -feature -ModemSimulator -accel on -memory 2048 -cores 4 \
  -skin 1280x720 -dpi-device 160
