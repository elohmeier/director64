#!/bin/sh
# The browser imports MPEG-1 program streams locally, without a server codec.
set -eu
out=${1:-build/web/tools}
mkdir -p "$out"
emcc -std=c11 -O2 -Ithird_party/pl_mpeg platforms/web/tools/mpeg_tool.c \
  -sMODULARIZE=1 -sEXPORT_NAME=createMpeg -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,worker,node \
  -sEXPORTED_FUNCTIONS=_d64m_open,_d64m_close,_d64m_width,_d64m_height,_d64m_time,_d64m_corrupt,_d64m_frame,_d64m_audio_streams,_d64m_sample_rate,_d64m_samples,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8 -sINCOMING_MODULE_JS_API=wasmBinary -o "$out/mpeg.js"
