#!/bin/sh
# libdragon's mkfont and mksprite as WebAssembly, for the browser importer's
# font stage (platforms/web/import/fonts.mjs). Built from the pinned sources
# unchanged: platforms/web/tools/subprocess_shim.h stands in for the
# subprocess mkfont starts, and mksprite_lossy_stub.c for the lossy codecs.
# Run in the pinned Emscripten image by `director64 web`.
#   sh platforms/web/tools/build-fonttools.sh OUT_DIR
set -e
L=third_party/libdragon
O=${1:-build/web/tools}
T=$O/fonttools-objects
mkdir -p $T
COMMON="-O2 -D__LIBDRAGON_INTERNAL_BUILD -I$L/include -Wno-unused-result -Wno-sign-compare"
LINK="-sMODULARIZE=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web,worker,node -sFORCE_FILESYSTEM=1 -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV,UTF8ToString,HEAPU8,HEAPU32 -sEXPORTED_FUNCTIONS=_main,_malloc,_free -sSTACK_SIZE=4MB -sINCOMING_MODULE_JS_API=wasmBinary,print,printErr,preRun,d64RunMksprite"
em++ -std=gnu++17 $COMMON -Wno-c++11-narrowing -Wno-narrowing -c $L/tools/common/shrinkler_compress.cpp -o $T/shrinkler.o
emcc -std=gnu11 $COMMON -c $L/tools/common/assetcomp.c -o $T/assetcomp.o
emcc -std=gnu11 $COMMON -c $L/tools/common/lz4_compress.c -o $T/lz4.o
emcc -std=gnu11 $COMMON -c $L/tools/common/aplib_compress.c -o $T/aplib.o
emcc -std=gnu11 $COMMON -c $L/tools/mksprite/mksprite.c -o $T/mksprite.o
emcc -std=gnu11 $COMMON -I$L/tools/mksprite -c platforms/web/tools/mksprite_lossy_stub.c -o $T/lossy.o
em++ $T/mksprite.o $T/lossy.o $T/assetcomp.o $T/lz4.o $T/aplib.o $T/shrinkler.o $LINK -sEXPORT_NAME=createMksprite -o $O/mksprite.js
emcc -std=gnu11 -O2 -w -c $L/tools/mkfont/freetype/FreeTypeAmalgam.c -o $T/freetype.o
emcc -std=gnu11 -O2 -w -I$L/tools/mkfont/plutosvg -I$L/tools/mkfont/freetype -c $L/tools/mkfont/plutosvg/plutosvg-amalgam.c -o $T/plutosvg.o
em++ -std=gnu++17 $COMMON -Wno-c++11-narrowing -Wno-narrowing -I$L/tools/mkfont/plutosvg -I$L/tools/mkfont/freetype \
  -include platforms/web/tools/subprocess_shim.h -c $L/tools/mkfont/mkfont.cpp -o $T/mkfont.o
em++ $T/mkfont.o $T/freetype.o $T/plutosvg.o $T/assetcomp.o $T/lz4.o $T/aplib.o $T/shrinkler.o $LINK -sEXPORT_NAME=createMkfont -o $O/mkfont.js
