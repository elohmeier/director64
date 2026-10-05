FROM debian:bookworm-slim@sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241

RUN dpkg --add-architecture i386 \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y \
        wine wine32:i386 xvfb xdotool x11-utils imagemagick \
        ffmpeg pulseaudio pulseaudio-utils libasound2-plugins:i386 \
        fonts-liberation python3 \
    && rm -rf /var/lib/apt/lists/*

ENV WINEARCH=win32 WINEDEBUG=-all DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1
LABEL org.director64.reference.contains-proprietary-content="false"
ENTRYPOINT ["python3", "/runner/reference_runner.py", "_capture"]
