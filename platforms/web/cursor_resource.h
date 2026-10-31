#ifndef DIRECTOR64_CURSOR_RESOURCE_H
#define DIRECTOR64_CURSOR_RESOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Generated from a pinned RT_CURSOR resource; each pixel retains both AND
// and XOR bits. Inversion acts on the actual destination, never a fake alpha.
typedef struct {
  unsigned resource, width, height, hot_x, hot_y;
  bool loaded;
  uint8_t pixels[64 * 64];
} wc_resource_cursor_t;
static inline unsigned wc_cursor_be16(const uint8_t *p) {
  return (unsigned)p[0] << 8 | p[1];
}
static inline bool wc_resource_cursor_decode(wc_resource_cursor_t *out, unsigned resource,
                                             const uint8_t *data, size_t length) {
  if (!data || length < 16 || memcmp(data, "D64C", 4) ||
      wc_cursor_be16(data+12) != 1 || wc_cursor_be16(data+14)) return false;
  unsigned w=wc_cursor_be16(data+4), h=wc_cursor_be16(data+6);
  unsigned x=wc_cursor_be16(data+8), y=wc_cursor_be16(data+10);
  if (!w || !h || w>64 || h>64 || x>=w || y>=h || length!=16u+w*h) return false;
  for (unsigned i=0;i<w*h;i++) if (data[16+i]>3) return false;
  memcpy(out->pixels,data+16,w*h);
  out->width=w;out->height=h;out->hot_x=x;out->hot_y=y;
  out->resource=resource;out->loaded=true;
  return true;
}
static inline uint32_t wc_resource_cursor_pixel(uint8_t mask, uint32_t background) {
  switch (mask) {
  case 0: return UINT32_C(0xff000000);
  case 1: return UINT32_MAX;
  case 2: return background;
  case 3: return background ^ UINT32_C(0x00ffffff);
  default: return background; // Decoder rejects this value.
  }
}
static inline uint32_t wc_resource_cursor_hash(const wc_resource_cursor_t *cursor) {
  uint32_t hash=UINT32_C(2166136261);
  unsigned header[]={cursor->width,cursor->height,cursor->hot_x,cursor->hot_y};
  for(unsigned i=0;i<4;i++) hash=(hash^header[i])*UINT32_C(16777619);
  for(unsigned i=0;i<cursor->width*cursor->height;i++)
    hash=(hash^cursor->pixels[i])*UINT32_C(16777619);
  return hash;
}
#endif
