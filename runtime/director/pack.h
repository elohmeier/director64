#ifndef DIRECTOR64_PACK_H
#define DIRECTOR64_PACK_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
// The image pack (src/director64/image_pack.py): every packed image once,
// and per scene the directory of what the scene draws, best first. The
// console opens it at boot, reads a scene's directory at entry and
// decompresses each image from its offset. Every field is big-endian.
enum {
  DG_PACK_HEADER_BYTES = 32,
  DG_PACK_ENTRY_BYTES = 28,
  DG_PACK_SCENE_BYTES = 8,
  // The ink the index entries are keyed under: no sprite ink takes it.
  DG_PACK_INK_AUTHORED = 255,
};
typedef struct {
  uint32_t index_count, scene_count, index_offset, scene_table_offset;
  uint32_t entries_offset, blobs_offset;
} dg_pack_header_t;
typedef struct {
  uint32_t key, offset, compressed, allocation, length;
  uint16_t width, height;
  // Per mille of the scene's most-drawn image's ticks this one was drawn
  // for; 0 for a row the score references but the walk never drew.
  uint16_t share;
  // DG_PACK_MASK_BAKED: the blob's coverage already carries its mask
  // member's cut (the D10 mask ink), so the console must not apply it.
  uint16_t flags;
} dg_pack_entry_t;
enum { DG_PACK_MASK_BAKED = 1 };
static inline uint32_t dg_pack_be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
// FNV-1a over the asset name and the ink, or'ed with one so zero can mark
// a free slot. The packer computes the same key (image_pack.image_key).
static inline uint32_t dg_image_key(const char *asset, unsigned ink) {
  uint32_t hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)asset; *p; p++)
    hash = (hash ^ *p) * 16777619u;
  hash = (hash ^ (uint32_t)ink) * 16777619u;
  return hash | 1u;
}
static inline bool dg_pack_header_parse(const uint8_t *data, dg_pack_header_t *out) {
  if (memcmp(data, "D64P", 4) != 0 || dg_pack_be32(data + 4) != 1) return false;
  out->index_count = dg_pack_be32(data + 8);
  out->scene_count = dg_pack_be32(data + 12);
  out->index_offset = dg_pack_be32(data + 16);
  out->scene_table_offset = dg_pack_be32(data + 20);
  out->entries_offset = dg_pack_be32(data + 24);
  out->blobs_offset = dg_pack_be32(data + 28);
  return out->index_offset >= DG_PACK_HEADER_BYTES &&
         out->entries_offset >= out->index_offset && out->blobs_offset >= out->entries_offset;
}
static inline void dg_pack_entry_parse(const uint8_t *data, dg_pack_entry_t *out) {
  out->key = dg_pack_be32(data);
  out->offset = dg_pack_be32(data + 4);
  out->compressed = dg_pack_be32(data + 8);
  out->allocation = dg_pack_be32(data + 12);
  out->length = dg_pack_be32(data + 16);
  out->width = (uint16_t)(data[20] << 8 | data[21]);
  out->height = (uint16_t)(data[22] << 8 | data[23]);
  out->share = (uint16_t)(data[24] << 8 | data[25]);
  out->flags = (uint16_t)(data[26] << 8 | data[27]);
}
#endif
