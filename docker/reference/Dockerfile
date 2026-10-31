FROM debian:bookworm-slim@sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241

RUN dpkg --add-architecture i386 \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends -y \
        wine wine32:i386 xvfb xdotool x11-utils imagemagick \
        ffmpeg pulseaudio pulseaudio-utils libasound2-plugins:i386 \
        gstreamer1.0-tools:i386 gstreamer1.0-plugins-base:i386 \
        gstreamer1.0-plugins-good:i386 gstreamer1.0-plugins-bad:i386 \
        gstreamer1.0-libav:i386 \
        fonts-liberation python3 \
    && rm -rf /var/lib/apt/lists/*

COPY check-codecs.sh /usr/local/bin/director64-check-codecs
RUN sh /usr/local/bin/director64-check-codecs
COPY entrypoint.py /usr/local/bin/director64-reference

ENV WINEARCH=win32 WINEDEBUG=-all DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2
LABEL org.director64.reference.contains-proprietary-content="false"
ENTRYPOINT ["python3", "/usr/local/bin/director64-reference"]
