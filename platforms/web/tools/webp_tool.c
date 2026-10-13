// The importer's image packer (platforms/web/import, "image" jobs): a stored
// image as lossless WebP in a "D64W" container (platforms/web/webp_image.h).
// Kept only when the runtime's decoder gives back exactly the stored bytes.
#include "bitmap_tiles.h"
#include "src/webp/encode.h"
#include "webp_image.h"
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

// The packed image, malloc'd, its length in `*out_length`; NULL when the
// image is no FDI1/FDIA/FDI2 or would not come back exactly.
EMSCRIPTEN_KEEPALIVE uint8_t *d64w_encode(const uint8_t *fdi, uint32_t length, int method,
                                          int quality, uint32_t *out_length) {
  *out_length = 0;
  if (length < 32 || be32(fdi + 4) != length)
    return NULL;
  bool wide = !memcmp(fdi, "FDI2", 4), planes = !memcmp(fdi, "FDIA", 4);
  if (!wide && !planes && memcmp(fdi, "FDI1", 4))
    return NULL;
  unsigned width = be16(fdi + 8), height = be16(fdi + 10);
  if (!width || !height || width > 16383 || height > 16383)
    return NULL;
  bool tiled = bitmap_needs_tiles(width, height);
  uint64_t plane = bitmap_plane_pixels(width, height);
  uint32_t alpha_offset = planes ? be32(fdi + 20) : 0;
  if (32u + plane * (wide ? 4u : 2u) > length || (planes && alpha_offset + plane > length))
    return NULL;
  uint8_t *rgba = malloc((size_t)width * height * 4);
  if (!rgba)
    return NULL;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      uint8_t *p = rgba + ((size_t)y * width + x) * 4;
      uint64_t slot = tiled ? bitmap_tile_index(width, x, y) : (uint64_t)y * width + x;
      if (wide) {
        memcpy(p, fdi + 32 + slot * 4, 4);
        continue;
      }
      unsigned word = be16(fdi + 32 + slot * 2);
      unsigned r = word >> 11 & 31, g = word >> 6 & 31, b = word >> 1 & 31;
      p[0] = (uint8_t)(r << 3 | r >> 2);
      p[1] = (uint8_t)(g << 3 | g >> 2);
      p[2] = (uint8_t)(b << 3 | b >> 2);
      p[3] = planes ? fdi[alpha_offset + slot] : (word & 1) ? 255 : 0;
    }
  WebPConfig config;
  WebPPicture picture;
  WebPMemoryWriter writer;
  uint8_t *out = NULL;
  if (!WebPConfigInit(&config) || !WebPPictureInit(&picture)) {
    free(rgba);
    return NULL;
  }
  config.lossless = 1;
  config.exact = 1;
  config.method = method;
  config.quality = (float)quality;
  picture.use_argb = 1;
  picture.width = (int)width;
  picture.height = (int)height;
  WebPMemoryWriterInit(&writer);
  picture.writer = WebPMemoryWrite;
  picture.custom_ptr = &writer;
  bool encoded = WebPPictureImportRGBA(&picture, rgba, (int)width * 4) &&
                 WebPEncode(&config, &picture);
  WebPPictureFree(&picture);
  free(rgba);
  if (encoded) {
    uint32_t total = 40u + (uint32_t)writer.size;
    out = malloc(total);
    if (out) {
      memcpy(out, "D64W", 4);
      out[4] = (uint8_t)(length >> 24);
      out[5] = (uint8_t)(length >> 16);
      out[6] = (uint8_t)(length >> 8);
      out[7] = (uint8_t)length;
      memcpy(out + 8, fdi, 32);
      memcpy(out + 40, writer.mem, writer.size);
      uint32_t check_length;
      uint8_t *check = dg_webp_fdi(out, total, &check_length);
      bool exact = check && check_length == length && !memcmp(check, fdi, length);
      free(check);
      if (exact)
        *out_length = total;
      else {
        free(out);
        out = NULL;
      }
    }
  }
  WebPMemoryWriterClear(&writer);
  return out;
}
EMSCRIPTEN_KEEPALIVE void d64w_free(void *p) { free(p); }
