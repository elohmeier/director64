#include "archive.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static void write32(uint8_t *p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static uint32_t crc(const uint8_t *p) {
  uint32_t c = UINT32_MAX;
  for (unsigned i = 0; i < ARCHIVE_BYTES; i++) {
    c ^= i >= 16 && i < 20 ? 0 : p[i];
    for (unsigned b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return c ^ UINT32_MAX;
}
int archive_file_id(const char *name) {
  char lower[32];
  size_t n = strlen(name);
  if (n >= sizeof(lower))
    return -1;
  for (size_t i = 0; i <= n; i++)
    lower[i] = (char)tolower((unsigned char)name[i]);
  if (n == 9 && !strncmp(lower, "vakt", 4) && lower[4] >= '1' &&
      lower[4] <= '7' && !strcmp(lower + 5, ".txt"))
    return lower[4] - '1';
  if (!strcmp(lower, "byggfil.txt"))
    return 7;
  if (!strcmp(lower, "musikfil.txt"))
    return 8;
  return -1;
}
static unsigned offset(const archive_t *a, unsigned file) {
  unsigned n = 0;
  for (unsigned i = 0; i < file; i++)
    n += a->lengths[i];
  return n;
}
static bool valid(const uint8_t *p) {
  unsigned version = read32(p + 4);
  if (memcmp(p, "F64D", 4) || (version != 1 && version != 2) || !read32(p + 8) ||
      read32(p + 16) != crc(p) || read32(p + 20) != 0x434f4d54 ||
      read32(p + 24) != ARCHIVE_FILES)
    return false;
  unsigned used = 0;
  for (unsigned i = 0; i < ARCHIVE_FILES; i++) {
    unsigned n = read32(p + 32 + i * 4);
    if (n > ARCHIVE_FILE_LIMIT || n > ARCHIVE_BYTES - ARCHIVE_HEADER - used)
      return false;
    used += n;
  }
  if (used != read32(p + 12))
    return false;
  for (unsigned i = 28; i < 32; i++)
    if (p[i])
      return false;
  // V2 reserves a 0/1 header word that a removed setting once wrote. The
  // nine files and their payload offsets are unchanged in both versions.
  if (version == 2 && read32(p + 68) > 1) return false;
  for (unsigned i = version == 2 ? 72 : 68; i < ARCHIVE_HEADER; i++)
    if (p[i])
      return false;
  for (unsigned i = ARCHIVE_HEADER; i < ARCHIVE_HEADER + used; i++)
    if (!p[i])
      return false;
  for (unsigned i = ARCHIVE_HEADER + used; i < ARCHIVE_BYTES; i++)
    if (p[i])
      return false;
  return true;
}
static void profiles(archive_t *a, const save_model_t *model) {
  unsigned pos = 0;
  for (unsigned i = 0; i < SAVE_PROFILES; i++) {
    const save_profile_t *p = &model->profiles[i];
    char text[512];
    int n = snprintf(text, sizeof(text), "%lu\r%lu\r[",
                     (unsigned long)p->feathers, (unsigned long)p->coins);
    for (unsigned j = 0; j < 35; j++)
      n += snprintf(text + n, sizeof(text) - (unsigned)n, "%s%u", j ? "," : "",
                    p->treasure[j]);
    n += snprintf(text + n, sizeof(text) - (unsigned)n, "]\r[");
    for (unsigned j = 0; j < 9; j++)
      n += snprintf(text + n, sizeof(text) - (unsigned)n, "%s%u", j ? "," : "",
                    p->map[j]);
    n += snprintf(text + n, sizeof(text) - (unsigned)n, "]\r");
    a->lengths[i] = (unsigned)n;
    memcpy(a->data + pos, text, (unsigned)n);
    pos += (unsigned)n;
  }
}
save_status_t archive_load(archive_t *a, save_backend_t backend) {
  memset(a, 0, sizeof(*a));
  a->backend = backend;
  a->active_slot = -1;
  a->status = SAVE_IO_ERROR;
  if (!backend.read || !backend.write)
    return a->status;
  uint8_t *block = malloc(ARCHIVE_BYTES);
  if (!block)
    return a->status;
  bool blank = true;
  for (unsigned slot = 0; slot < 2; slot++) {
    if (!backend.read(backend.context, slot * ARCHIVE_BYTES, block,
                      ARCHIVE_BYTES)) {
      free(block);
      return a->status;
    }
    bool zero = true, erased = true;
    for (unsigned i = 0; i < ARCHIVE_BYTES; i++) {
      zero &= block[i] == 0;
      erased &= block[i] == 255;
    }
    blank &= zero || erased;
    if (valid(block) &&
        (a->active_slot < 0 || read32(block + 8) > a->generation)) {
      a->generation = read32(block + 8);
      a->active_slot = (int)slot;
      for (unsigned i = 0; i < ARCHIVE_FILES; i++)
        a->lengths[i] = read32(block + 32 + i * 4);
      memcpy(a->data, block + ARCHIVE_HEADER, sizeof(a->data));
    }
  }
  free(block);
  if (a->active_slot >= 0)
    return a->status = SAVE_VALID;
  save_store_t legacy;
  if (save_load(&legacy, backend) == SAVE_VALID) {
    profiles(a, &legacy.model);
    a->generation = legacy.generation;
    a->active_slot = legacy.active_slot;
    a->migrated = true;
    return a->status = SAVE_VALID;
  }
  if (blank) {
    save_model_t empty = {0};
    profiles(a, &empty);
    return a->status = SAVE_BLANK;
  }
  return a->status = SAVE_CORRUPT;
}
bool archive_read(archive_t *a, const char *name, char *out, unsigned cap,
                  unsigned *length) {
  int id = archive_file_id(name);
  if (id < 0 || !cap || a->lengths[id] >= cap ||
      (a->status != SAVE_VALID && a->status != SAVE_BLANK))
    return false;
  *length = a->lengths[id];
  memcpy(out, a->data + offset(a, (unsigned)id), *length);
  out[*length] = 0;
  return true;
}
static bool commit(archive_t *a, unsigned id, const char *text,
                    unsigned length) {
  unsigned start = offset(a, id), used = offset(a, ARCHIVE_FILES);
  unsigned previous = id < ARCHIVE_FILES ? a->lengths[id] : 0;
  if (a->generation == UINT32_MAX) {
    a->status = SAVE_FULL;
    return false;
  }
  uint8_t *block = calloc(1, ARCHIVE_BYTES), *verify = malloc(ARCHIVE_BYTES);
  if (!block || !verify) {
    free(block);
    free(verify);
    return false;
  }
  memcpy(block, "F64D", 4);
  write32(block + 4, 2);
  write32(block + 8, a->generation + 1);
  write32(block + 12, used - previous + length);
  write32(block + 20, 0x434f4d54);
  write32(block + 24, ARCHIVE_FILES);
  for (unsigned i = 0; i < ARCHIVE_FILES; i++)
    write32(block + 32 + i * 4, i == (unsigned)id ? length : a->lengths[i]);
  memcpy(block + ARCHIVE_HEADER, a->data, start);
  if (length) memcpy(block + ARCHIVE_HEADER + start, text, length);
  memcpy(block + ARCHIVE_HEADER + start + length,
         a->data + start + previous, used - start - previous);
  write32(block + 16, crc(block));
  unsigned slot = a->active_slot == 0 ? 1 : 0;
  bool ok = a->backend.write(a->backend.context, slot * ARCHIVE_BYTES, block,
                             ARCHIVE_BYTES) &&
            a->backend.read(a->backend.context, slot * ARCHIVE_BYTES, verify,
                            ARCHIVE_BYTES) &&
            !memcmp(block, verify, ARCHIVE_BYTES);
  if (ok) {
    memcpy(a->data, block + ARCHIVE_HEADER, sizeof(a->data));
    if (id < ARCHIVE_FILES) a->lengths[id] = length;
    a->active_slot = (int)slot;
    a->generation++;
    a->migrated = false;
    a->status = SAVE_VALID;
  }
  // A failed write leaves the active generation and readable RAM intact.
  free(block);
  free(verify);
  return ok;
}
bool archive_write(archive_t *a, const char *name, const char *text,
                   unsigned length) {
  int id = archive_file_id(name);
  if (id < 0 || length > ARCHIVE_FILE_LIMIT || memchr(text, 0, length) ||
      (a->status != SAVE_VALID && a->status != SAVE_BLANK)) return false;
  unsigned start = offset(a, (unsigned)id), used = offset(a, ARCHIVE_FILES);
  if (length > sizeof(a->data) - (used - a->lengths[id])) return false;
  if (a->lengths[id] == length && !memcmp(a->data + start, text, length)) return true;
  return commit(a, (unsigned)id, text, length);
}
