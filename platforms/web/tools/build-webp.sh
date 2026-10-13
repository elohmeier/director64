#!/bin/sh
# The importer's image packer as WebAssembly (platforms/web/tools/webp_tool.c):
# the vendored libwebp encoder and decoder, so every packed image is checked
# against the decoder the runtime uses. Run in the pinned Emscripten image by
# `director64 web`.
#   sh platforms/web/tools/build-webp.sh OUT_DIR
set -e
W=third_party/libwebp
O=${1:-build/web/tools}
mkdir -p $O
SOURCES=$(ls $W/src/dec/*.c $W/src/dsp/*.c $W/src/enc/*.c $W/src/utils/*.c $W/sharpyuv/*.c | grep -v "_mips\|_msa\|_neon")
emcc -std=gnu11 -O2 -I$W -Iruntime/director -Iplatforms/web $SOURCES platforms/web/webp_image.c platforms/web/tools/webp_tool.c \
  -sMODULARIZE=1 -sEXPORT_NAME=createWebp -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,worker,node \
  -sEXPORTED_FUNCTIONS=_d64w_encode,_d64w_free,_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
  -sINCOMING_MODULE_JS_API=wasmBinary -o $O/webp.js
