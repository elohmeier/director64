#include "packs.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

char *read_file(const char *path, size_t *length) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char *data = NULL;
  if (fseek(f, 0, SEEK_END) == 0) {
    long size = ftell(f);
    if (size >= 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)size + 1))) {
      if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
      } else {
        data[size] = 0;
        if (length)
          *length = (size_t)size;
      }
    }
  }
  fclose(f);
  return data;
}

static int by_name(const void *a, const void *b) {
  return strcmp(((const pack_entry_t *)a)->name, ((const pack_entry_t *)b)->name);
}

static AAssetManager *assets;

void packs_use(AAssetManager *manager) { assets = manager; }

char *game_asset(const char *name, size_t *length) {
  char path[256];
  snprintf(path, sizeof(path), "game/%s", name);
  AAsset *asset = assets ? AAssetManager_open(assets, path, AASSET_MODE_STREAMING) : NULL;
  if (!asset)
    return NULL;
  off64_t size = AAsset_getLength64(asset);
  char *data = malloc((size_t)size + 1);
  if (data && AAsset_read(asset, data, (size_t)size) != size) {
    free(data);
    data = NULL;
  }
  AAsset_close(asset);
  if (!data)
    return NULL;
  data[size] = 0;
  if (length)
    *length = (size_t)size;
  return data;
}

bool pack_open(pack_t *pack, const char *kind, char *error, size_t error_cap) {
  memset(pack, 0, sizeof(*pack));
  pack->fd = -1;
  char path[256];
  snprintf(path, sizeof(path), "%s.json", kind);
  size_t length = 0;
  char *text = game_asset(path, &length);
  if (!text) {
    snprintf(error, error_cap, "%s.json is missing", kind);
    return false;
  }
  json_t json;
  bool ok = json_parse(&json, text, length) && json.tokens[0].type == JSON_OBJECT;
  if (ok) {
    unsigned members = json.tokens[0].size / 2;
    pack->entries = calloc(members ? members : 1, sizeof(pack_entry_t));
    size_t name_bytes = 0;
    for (unsigned i = 1; i < json.count; i = json_next(&json, i + 1))
      name_bytes += json.tokens[i].end - json.tokens[i].start + 1;
    pack->names = malloc(name_bytes + 1);
    ok = pack->entries && pack->names;
    char *names = pack->names;
    for (unsigned i = 1; ok && i < json.count; i = json_next(&json, i + 1)) {
      unsigned value = i + 1;
      if (value >= json.count || json.tokens[value].type != JSON_ARRAY ||
          json.tokens[value].size < 2) {
        ok = false;
        break;
      }
      pack_entry_t *e = &pack->entries[pack->count++];
      size_t n = json_string(&json, i, names, json.tokens[i].end - json.tokens[i].start + 1);
      e->name = names;
      names += n + 1;
      e->offset = (uint64_t)json_number(&json, json_item(&json, value, 0));
      e->length = (uint32_t)json_number(&json, json_item(&json, value, 1));
      e->extra = json.tokens[value].size > 2
                     ? (uint32_t)json_number(&json, json_item(&json, value, 2))
                     : 0;
    }
  }
  json_free(&json);
  free(text);
  if (!ok) {
    snprintf(error, error_cap, "%s.json is not a pack index", kind);
    pack_close(pack);
    return false;
  }
  qsort(pack->entries, pack->count, sizeof(pack_entry_t), by_name);
  snprintf(path, sizeof(path), "game/%s.bin", kind);
  pack->asset = assets ? AAssetManager_open(assets, path, AASSET_MODE_BUFFER) : NULL;
  if (!pack->asset) {
    snprintf(error, error_cap, "%s.bin is missing", kind);
    pack_close(pack);
    return false;
  }
  pack->bytes = (size_t)AAsset_getLength64(pack->asset);
  if (pack->bytes) {
    // Stored uncompressed, the asset is a range of the APK: mapped in place,
    // and readable through the APK's descriptor.
    off64_t start = 0, size = 0;
    pack->fd = AAsset_openFileDescriptor64(pack->asset, &start, &size);
    pack->base = (uint64_t)start;
    pack->blob = AAsset_getBuffer(pack->asset);
    if (!pack->blob || pack->fd < 0) {
      snprintf(error, error_cap, "%s.bin is compressed in the APK", kind);
      pack_close(pack);
      return false;
    }
  }
  for (unsigned i = 0; i < pack->count; i++)
    if (pack->entries[i].offset + pack->entries[i].length > pack->bytes) {
      snprintf(error, error_cap, "%s.bin is shorter than its index", kind);
      pack_close(pack);
      return false;
    }
  return true;
}

void pack_close(pack_t *pack) {
  if (pack->asset)
    AAsset_close(pack->asset);
  if (pack->fd >= 0)
    close(pack->fd);
  free(pack->entries);
  free(pack->names);
  memset(pack, 0, sizeof(*pack));
  pack->fd = -1;
}

const pack_entry_t *pack_find(const pack_t *pack, const char *name) {
  if (!name || !pack->count)
    return NULL;
  pack_entry_t key = {.name = name};
  return bsearch(&key, pack->entries, pack->count, sizeof(pack_entry_t), by_name);
}

uint32_t pack_read(const pack_t *pack, const char *name, uint8_t *out, uint32_t cap) {
  const pack_entry_t *e = pack_find(pack, name);
  if (!e)
    return 0;
  const uint8_t *data = pack_data(pack, e);
  if (e->length < 8 || memcmp(data, "D64Z", 4)) {
    if (out && cap >= e->length)
      memcpy(out, data, e->length);
    return e->length;
  }
  uint32_t length = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 |
                    (uint32_t)data[6] << 8 | data[7];
  if (!out || cap < length)
    return length;
  z_stream z = {0};
  if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
    return 0;
  z.next_in = (Bytef *)(data + 8);
  z.avail_in = e->length - 8;
  z.next_out = out;
  z.avail_out = length;
  int status = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  return status == Z_STREAM_END && z.total_out == length ? length : 0;
}
