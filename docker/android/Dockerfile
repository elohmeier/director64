# The Android TV player's toolchain (docs/android.md): a JDK, the SDK's
# platform and build tools and the NDK, at pinned versions. The APK builds
# without Gradle (src/director64/android.py), so the build runs offline.
FROM docker.io/library/eclipse-temurin:21-jdk-noble@sha256:b468c3fc688b14450571494f588bd939378e7fd542ed5a73f8efc13f17872a87 AS sdk

ARG CMDLINE_TOOLS=commandlinetools-linux-16111833_latest.zip
ARG CMDLINE_TOOLS_SHA256=0877a1d048fe4a24efe2eff536ca4223f7adeb58648bb81909d33c446918cfa8
ENV ANDROID_HOME=/opt/android-sdk
RUN apt-get update \
 && apt-get install -y --no-install-recommends unzip curl ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && curl -fsSLo /tmp/tools.zip "https://dl.google.com/android/repository/${CMDLINE_TOOLS}" \
 && echo "${CMDLINE_TOOLS_SHA256}  /tmp/tools.zip" | sha256sum -c - \
 && mkdir -p "$ANDROID_HOME/cmdline-tools" \
 && unzip -q /tmp/tools.zip -d "$ANDROID_HOME/cmdline-tools" \
 && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest" \
 && rm /tmp/tools.zip
# sdkmanager verifies every package against the repository's checksums.
RUN yes | "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --licenses >/dev/null \
 && "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --install \
      "platforms;android-35" "build-tools;36.0.0" "ndk;28.2.13676358" "platform-tools" \
 && chmod -R a+rX "$ANDROID_HOME"
ENV ANDROID_NDK=/opt/android-sdk/ndk/28.2.13676358 \
    BUILD_TOOLS=/opt/android-sdk/build-tools/36.0.0 \
    ANDROID_JAR=/opt/android-sdk/platforms/android-35/android.jar
WORKDIR /workdir

# An emulator for development checks: Android 14, as the EF-62N runs. The
# Android TV images are user builds whose adbd refuses the emulator's key, so
# this is the Google APIs image. Not needed to build; run with --device /dev/kvm.
FROM sdk AS emulator
RUN apt-get update \
 && apt-get install -y --no-install-recommends libpulse0 libnss3 libxcomposite1 libxcursor1 \
      libxi6 libxtst6 libxdamage1 libxrandr2 libgl1 libegl1 libasound2t64 libdbus-1-3 \
 && rm -rf /var/lib/apt/lists/* \
 && "$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --install \
      "emulator" "system-images;android-34;google_apis;x86_64" \
 && chmod -R a+rX "$ANDROID_HOME"
