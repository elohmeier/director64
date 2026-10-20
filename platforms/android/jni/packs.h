#ifndef DIRECTOR64_ANDROID_PACKS_H
#define DIRECTOR64_ANDROID_PACKS_H
// The game's asset packs, as the browser's cache holds them
// (platforms/web/site/cache.js), compacted at Android staging time. Each
// <kind>.json is {version: 1, blob: kind, entries: {name: [offset, length, ...]}}.
// Streams use blob "audio" too. Names may alias a single stored payload.
// The .bin files ship in the APK
// under assets/game/, stored uncompressed, so a blob maps in place and the
// media extractor reads a stream through the APK's file descriptor.
#include <android/asset_manager.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *name;
  uint64_t offset;
  uint32_t length;
  // A third index field: a playFile stream's length in milliseconds.
  uint32_t extra;
} pack_entry_t;

typedef struct {
  AAsset *asset;
  // The APK and where the blob starts in it.
  int fd;
  uint64_t base;
  const uint8_t *blob;
  size_t bytes;
  pack_entry_t *entries; // sorted by name
  unsigned count;
  char *names;
} pack_t;

// The APK's assets; set once before any pack opens.
void packs_use(AAssetManager *);
bool pack_open(pack_t *, const char *kind, char *error, size_t error_cap);
void pack_close(pack_t *);
const pack_entry_t *pack_find(const pack_t *, const char *name);
static inline const uint8_t *pack_data(const pack_t *pack, const pack_entry_t *entry) {
  return pack->blob + entry->offset;
}
// A stored asset's plain length and, when `out` holds it, its bytes: the
// importer keeps images deflated ("D64Z", the raw length big-endian, raw
// deflate; compiler/src/convert/files.rs). 0 when missing or corrupt.
uint32_t pack_read(const pack_t *, const char *name, uint8_t *out, uint32_t cap);
// A whole asset under assets/game/, NUL-terminated; NULL when missing.
char *game_asset(const char *name, size_t *length);
// A whole file, NUL-terminated; NULL on failure.
char *read_file(const char *path, size_t *length);
#endif
