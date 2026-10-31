#!/bin/sh
# Exercise the same 32-bit plugins used by Wine's 32-bit playback engine.
# Only a generated test pattern and tone are used; no game media is in the image.
set -eu
test "$(dpkg-query -W -f='${Architecture}' gstreamer1.0-tools)" = i386
for element in decodebin mpegpsdemux avdec_mpegvideo avdec_mp2float; do
    gst-inspect-1.0 "$element" >/dev/null
done
codec_test=$(mktemp -d)
trap 'rm -rf "$codec_test"' EXIT HUP INT TERM
ffmpeg -hide_banner -loglevel error -filter_threads 1 \
    -f lavfi -i testsrc2=size=160x120:rate=25 \
    -f lavfi -i sine=frequency=440:sample_rate=44100 -t 0.4 \
    -c:v mpeg1video -threads 1 -c:a mp2 -f mpeg "$codec_test/test.mpg"
timeout 15s gst-launch-1.0 -q filesrc location="$codec_test/test.mpg" \
    ! decodebin name=decoded \
    decoded. ! queue ! videoconvert ! video/x-raw ! fakesink sync=false \
    decoded. ! queue ! audioconvert ! audio/x-raw ! fakesink sync=false
