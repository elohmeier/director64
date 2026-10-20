#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#include "../../platforms/android/jni/packs.c"

struct AAssetManager { const char *root; };
struct AAsset {
  FILE *file;
  uint8_t *data;
  size_t length, cursor;
  bool compressed;
};
static unsigned open_assets;

AAsset *AAssetManager_open(AAssetManager *manager, const char *path, int mode) {
  (void)mode;
  char filename[4096];
  snprintf(filename, sizeof(filename), "%s/%s", manager->root, path);
  FILE *input = fopen(filename, "rb");
  if (!input) return NULL;
  assert(!fseek(input, 0, SEEK_END));
  long size = ftell(input);
  assert(size >= 0 && !fseek(input, 0, SEEK_SET));
  AAsset *asset = calloc(1, sizeof(*asset));
  assert(asset);
  asset->length = (size_t)size;
  asset->data = malloc(asset->length ? asset->length : 1);
  assert(asset->data && fread(asset->data, 1, asset->length, input) == asset->length);
  fclose(input);
  // A blob lives at a nonzero offset in the APK descriptor. Keep the buffer
  // relative to its asset but make descriptor readers include that offset.
  asset->file = tmpfile();
  assert(asset->file);
  const char prefix[] = "synthetic APK prefix";
  assert(fwrite(prefix, 1, sizeof(prefix), asset->file) == sizeof(prefix));
  assert(fwrite(asset->data, 1, asset->length, asset->file) == asset->length);
  assert(!fflush(asset->file));
  snprintf(filename, sizeof(filename), "%s/%s.compressed", manager->root, path);
  asset->compressed = !access(filename, F_OK);
  open_assets++;
  return asset;
}

int64_t AAsset_getLength64(AAsset *asset) { return (int64_t)asset->length; }
int AAsset_read(AAsset *asset, void *out, size_t size) {
  size_t left = asset->length - asset->cursor;
  if (size > left) size = left;
  memcpy(out, asset->data + asset->cursor, size);
  asset->cursor += size;
  return (int)size;
}
void AAsset_close(AAsset *asset) {
  assert(open_assets);
  open_assets--;
  fclose(asset->file);
  free(asset->data);
  free(asset);
}
int AAsset_openFileDescriptor64(AAsset *asset, int64_t *start, int64_t *size) {
  if (asset->compressed) return -1;
  *size = (int64_t)asset->length;
  *start = sizeof("synthetic APK prefix");
  return dup(fileno(asset->file));
}
const void *AAsset_getBuffer(AAsset *asset) { return asset->data; }

int main(int argc, char **argv) {
  assert(argc >= 3);
  AAssetManager manager = {argv[1]};
  packs_use(&manager);
  pack_t pack;
  char error[256];
  if (!pack_open(&pack, argv[2], error, sizeof(error))) {
    assert(!open_assets);
    fprintf(stderr, "%s\n", error);
    return 2;
  }
  printf("{\"count\":%u,\"entries\":[", pack.count);
  for (int i = 3; i < argc; i++) {
    const pack_entry_t *entry = pack_find(&pack, argv[i]);
    assert(entry);
    uint8_t *bytes = malloc(entry->length ? entry->length : 1);
    assert(bytes);
    assert(pread(pack.fd, bytes, entry->length, (off_t)(pack.base + entry->offset)) ==
           (ssize_t)entry->length);
    assert(!memcmp(bytes, pack_data(&pack, entry), entry->length));
    uint32_t length = pack_read(&pack, argv[i], NULL, 0);
    uint8_t *plain = malloc(length ? length : 1);
    assert(plain && pack_read(&pack, argv[i], plain, length) == length);
    printf("%s[%llu,%u,%u,%lu,%u]", i == 3 ? "" : ",",
           (unsigned long long)entry->offset, entry->length, entry->extra,
           (unsigned long)crc32(0, bytes, entry->length), length);
    free(bytes);
    free(plain);
  }
  assert(!pack_find(&pack, "missing synthetic asset"));
  printf("]}\n");
  pack_close(&pack);
  assert(!open_assets && pack.fd == -1);
  return 0;
}
