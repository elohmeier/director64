#ifndef DIRECTOR64_WEB_WEBP_IMAGE_H
#define DIRECTOR64_WEB_WEBP_IMAGE_H
// Stored images the importer packs as lossless WebP: "D64W", the stored
// image's length (u32 big-endian), its 32-byte FDI header, then a WebP of
// its pixels as RGBA8888 (FDI1/FDIA's 5-bit channels expanded, the coverage
// bit or plane as alpha). Decoding gives back exactly the stored bytes.
#include <stdbool.h>
#include <stdint.h>

bool dg_webp_packed(const uint8_t *data, uint32_t length);
// The stored image, malloc'd, its length in `*length`; NULL when damaged.
uint8_t *dg_webp_fdi(const uint8_t *data, uint32_t length, uint32_t *out_length);
#endif
