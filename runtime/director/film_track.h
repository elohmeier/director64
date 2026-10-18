#ifndef DIRECTOR64_FILM_TRACK_H
#define DIRECTOR64_FILM_TRACK_H
#include "director.h"
#if DG_D10
// Compact native-film deltas. A track starts from an all-zero dg_spec_t.
// Groups: member, x, y, width, height, ink/blend, type/flags, fore/back,
// thickness/stretch/trails, loc_z, foreRGB, backRGB, rotation, skew.
static inline uint16_t dg_film_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static inline uint32_t dg_film_u32(const uint8_t *p) {
  return (uint32_t)dg_film_u16(p) | (uint32_t)dg_film_u16(p + 2) << 16;
}
static inline bool dg_film_track_read(const dg_film_track_t *track, uint32_t *offset,
                                      uint16_t *frame, dg_spec_t *state) {
  static const uint8_t sizes[14] = {4,2,2,2,2,2,2,2,3,2,4,4,4,4};
  if (!track || !track->data || *offset > track->size || track->size - *offset < 4)
    return false;
  const uint8_t *p = track->data + *offset;
  uint16_t mask = dg_film_u16(p + 2);
  if (!mask || (mask & 0xc000)) return false;
  uint32_t bytes = 4;
  for (unsigned i = 0; i < 14; i++) if (mask & (1u << i)) bytes += sizes[i];
  if (bytes > track->size - *offset) return false;
  *frame = dg_film_u16(p); p += 4;
  for (unsigned i = 0; i < 14; i++) if (mask & (1u << i)) {
    switch (i) {
      case 0: state->member = dg_film_u32(p); break;
      case 1: state->x = (int16_t)dg_film_u16(p); break;
      case 2: state->y = (int16_t)dg_film_u16(p); break;
      case 3: state->width = (int16_t)dg_film_u16(p); break;
      case 4: state->height = (int16_t)dg_film_u16(p); break;
      case 5: state->ink = p[0]; state->blend = p[1]; break;
      case 6: state->type = p[0]; state->flags = p[1]; break;
      case 7: state->fore = p[0]; state->back = p[1]; break;
      case 8: state->thickness = p[0]; state->stretch = p[1]; state->trails = p[2]; break;
      case 9: state->loc_z = (int16_t)dg_film_u16(p); break;
      case 10: state->fore_rgb = dg_film_u32(p); break;
      case 11: state->back_rgb = dg_film_u32(p); break;
      case 12: state->rotation = (int32_t)dg_film_u32(p); break;
      case 13: state->skew = (int32_t)dg_film_u32(p); break;
    }
    p += sizes[i];
  }
  *offset += bytes;
  return true;
}
#endif
#endif
