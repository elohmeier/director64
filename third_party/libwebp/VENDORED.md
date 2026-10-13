# libwebp 1.5.0 (vendored)

The library sources (`src/dec`, `src/dsp`, `src/enc`, `src/utils`,
`src/webp`, `sharpyuv`) of libwebp 1.5.0, unmodified, from
https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.5.0.tar.gz
(sha256 7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c).
BSD 3-Clause (`COPYING`) with Google's patent grant (`PATENTS`).

The browser and Android TV runtimes compile the decoder to unpack images
the importer stores as lossless WebP (`platforms/web/webp_image.c`); the
importer's `webp` tool compiles the encoder (`platforms/web/tools`).
