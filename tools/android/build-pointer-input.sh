#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
docker run --rm --network=none --user "$(id -u):$(id -g)" \
  --volume "$PWD:/workdir" --workdir /workdir director64-android:local sh -ec '
    mkdir -p build/tools/android-pointer-input/classes
    javac --release 11 -cp "$ANDROID_JAR" -d build/tools/android-pointer-input/classes \
      tools/android/DirectorPointerInput.java
    "$BUILD_TOOLS/d8" --release --min-api 28 --lib "$ANDROID_JAR" \
      --output build/tools/android-pointer-input/pointer-input.jar \
      build/tools/android-pointer-input/classes/DirectorPointerInput.class
  '
