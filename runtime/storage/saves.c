// The game's files, as its Lingo reads and writes them: the saves it may
// write, kept in a 128 KiB save image the host makes durable, and the
// read-only files the installed game would have found beside it. What a game
// has is data in its game.toml [player] table, generated into game_profile.h.
//
// The save image holds two 64 KiB generations; a write commits a whole new
// one to the slot not in use and reads it back, so a failed write leaves the
// previous generation. A generation is a 32-byte header ("D64S", version,
// generation, used bytes, CRC-32 with the CRC field as zero, entry count)
// and its entries: a name length byte, the lowercase name, a u32 length and
// the bytes, all integers big-endian.
#include "game.h"
#include "game_profile.h"
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { SLOT = 65536, HEADER = 32, NAME = 64 };

#ifdef GAME_SAVES
static const char *const saves[] = {GAME_SAVES};
#else
static const char *const saves[] = {NULL};
#endif
#ifdef GAME_FILES
static const char *const files[][2] = {GAME_FILES};
#else
static const char *const files[][2] = {{NULL, NULL}};
#endif
#ifdef GAME_FOLDERS
static const char *const folders[][2] = {GAME_FOLDERS};
#else
static const char *const folders[][2] = {{NULL, NULL}};
#endif
#define COUNT(a) (sizeof(a) / sizeof(*(a)))

static struct {
  save_backend_t backend;
  save_status_t status;
  int slot;
  uint32_t generation, used, count;
  uint8_t data[SLOT - HEADER];
} store;

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
static uint32_t crc(const uint8_t *p) {
  uint32_t c = UINT32_MAX;
  for (unsigned i = 0; i < SLOT; i++) {
    c ^= i >= 16 && i < 20 ? 0 : p[i];
    for (unsigned b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return c ^ UINT32_MAX;
}

// A name against one pattern, without case: ? any character, # a digit,
// * any run.
static bool glob(const char *pattern, const char *pattern_end, const char *name) {
  for (; pattern < pattern_end; pattern++, name++) {
    if (*pattern == '*') {
      for (const char *rest = name;; rest++) {
        if (glob(pattern + 1, pattern_end, rest)) return true;
        if (!*rest) return false;
      }
    }
    if (!*name) return false;
    if (*pattern == '#' ? !isdigit((unsigned char)*name)
        : *pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*name))
      return false;
  }
  return !*name;
}
// Whether `name` matches an entry's alternatives ("a|b"). A literal match
// names the file by the entry's first spelling; a wildcard one by its own.
static bool matches(const char *entry, const char *name, char *canonical) {
  for (const char *start = entry;;) {
    const char *end = strchr(start, '|');
    if (!end) end = start + strlen(start);
    if (glob(start, end, name)) {
      if (canonical) {
        bool literal = !strpbrk(entry, "?#*");
        const char *source = literal ? entry : name;
        size_t n = literal ? strcspn(entry, "|") : strlen(name);
        for (size_t i = 0; i < n; i++) canonical[i] = (char)tolower((unsigned char)source[i]);
        canonical[n] = 0;
      }
      return true;
    }
    if (!*end) return false;
    start = end + 1;
  }
}
static bool save_name(const char *name, char canonical[NAME]) {
  // Paths are virtual. Legacy profiles retain their historical basename
  // saves; games with per-player folders opt into scoped names. Never accept
  // parent traversal, and fold Windows/Mac separators consistently.
  char normalized[256];
  size_t used = 0;
  if (name[0] && name[1] == ':') name += 2;
  while (*name == '/' || *name == '\\' || *name == ':') name++;
  for (; *name; name++) {
    if (used + 1 >= sizeof(normalized)) return false;
    normalized[used++] = (*name == '\\' || *name == ':') ? '/' : (char)tolower((unsigned char)*name);
  }
  normalized[used] = 0;
  for (const char *p = normalized; *p;) {
    size_t n = strcspn(p, "/");
    if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
    p += n;
    if (*p) p++;
  }
#ifdef GAME_SAVE_PATHS
  name = normalized;
  if (used && normalized[used - 1] == '/' && used < NAME) {
    memcpy(canonical, normalized, used + 1);
    return true; // Persistent empty-directory entry, never a host folder.
  }
#else
  name = strrchr(normalized, '/');
  name = name ? name + 1 : normalized;
#endif
  if (strlen(name) >= NAME) return false;
  for (unsigned i = 0; i < COUNT(saves); i++)
    if (saves[i] && matches(saves[i], name, canonical)) return true;
  return false;
}

// The entry of `name` in `data`: its name byte's offset, or -1.
static long find(const uint8_t *data, uint32_t count, const char *name) {
  size_t n = strlen(name);
  long at = 0;
  for (uint32_t i = 0; i < count; i++) {
    unsigned length = data[at];
    if (length == n && !memcmp(data + at + 1, name, n)) return at;
    at += 1 + length + 4 + get32(data + at + 1 + length);
  }
  return -1;
}
static bool valid(const uint8_t *p) {
  if (memcmp(p, "D64S", 4) || get32(p + 4) != 1 || get32(p + 12) > SLOT - HEADER ||
      get32(p + 16) != crc(p))
    return false;
  uint32_t used = get32(p + 12), at = 0;
  for (uint32_t i = 0; i < get32(p + 20); i++) {
    if (at + 1 > used) return false;
    unsigned n = p[HEADER + at];
    if (!n || n >= NAME || at + 1 + n + 4 > used) return false;
    uint32_t length = get32(p + HEADER + at + 1 + n);
    if (length > used - (at + 1 + n + 4)) return false;
    at += 1 + n + 4 + length;
  }
  return at == used;
}

save_status_t game_save_load(save_backend_t backend) {
  memset(&store, 0, sizeof(store));
  store.backend = backend;
  store.slot = -1;
  if (!saves[0]) return store.status = SAVE_BLANK;
  store.status = SAVE_IO_ERROR;
  uint8_t *block = malloc(SLOT);
  if (!block || !backend.read || !backend.write) {
    free(block);
    return store.status;
  }
  bool blank = true;
  for (int slot = 0; slot < 2; slot++) {
    if (!backend.read(backend.context, (size_t)slot * SLOT, block, SLOT)) {
      free(block);
      return store.status;
    }
    // A slot of another format (erased, or an older layout) counts as
    // blank; a damaged generation of this one does not.
    blank &= memcmp(block, "D64S", 4) != 0;
    if (valid(block) && (store.slot < 0 || get32(block + 8) > store.generation)) {
      store.slot = slot;
      store.generation = get32(block + 8);
      store.used = get32(block + 12);
      store.count = get32(block + 20);
      memcpy(store.data, block + HEADER, sizeof(store.data));
    }
  }
  free(block);
  return store.status = store.slot >= 0 ? SAVE_VALID : blank ? SAVE_BLANK : SAVE_CORRUPT;
}
save_status_t game_save_status(void) { return store.status; }
unsigned game_save_generation(void) { return store.generation; }

// The nth saved file's name (from 0), or NULL past the last.
const char *game_save_name(unsigned n) {
  static char name[NAME];
  long at = 0;
  for (uint32_t i = 0; i < store.count; i++) {
    unsigned length = store.data[at];
    if (i == n) {
      memcpy(name, store.data + at + 1, length);
      name[length] = 0;
      return name;
    }
    at += 1 + length + 4 + get32(store.data + at + 1 + length);
  }
  return NULL;
}

bool game_read_file(const char *name, char *out, unsigned cap, unsigned *length) {
  char canonical[NAME];
  if (save_name(name, canonical) && (store.status == SAVE_VALID || store.status == SAVE_BLANK)) {
    long at = find(store.data, store.count, canonical);
    if (at >= 0) {
      unsigned n = store.data[at];
      uint32_t size = get32(store.data + at + 1 + n);
      if (size && size < cap) {
        memcpy(out, store.data + at + 1 + n + 4, size);
        out[size] = 0;
        *length = size;
        return true;
      }
    }
  }
  const char *base = name;
  for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
  for (unsigned i = 0; i < COUNT(files); i++)
    if (files[i][0] && matches(files[i][0], base, NULL)) {
      size_t size = strlen(files[i][1]);
      if (size >= cap) return false;
      memcpy(out, files[i][1], size + 1);
      *length = (unsigned)size;
      return true;
    }
  return false;
}

bool game_write_file(const char *name, const char *data, unsigned length) {
  char canonical[NAME];
  if (!save_name(name, canonical) || (store.status != SAVE_VALID && store.status != SAVE_BLANK))
    return false;
  if (store.generation == UINT32_MAX) {
    store.status = SAVE_FULL;
    return false;
  }
  uint8_t *block = calloc(1, SLOT), *check = malloc(SLOT);
  if (!block || !check) {
    free(block);
    free(check);
    return false;
  }
  // The new generation: every other entry as it was, this one replaced or
  // appended.
  long old = find(store.data, store.count, canonical);
  uint32_t used = 0, count = 0;
  long at = 0;
  for (uint32_t i = 0; i < store.count; i++) {
    unsigned n = store.data[at];
    uint32_t size = 1 + n + 4 + get32(store.data + at + 1 + n);
    if (at != old) {
      memcpy(block + HEADER + used, store.data + at, size);
      used += size;
      count++;
    }
    at += size;
  }
  size_t n = strlen(canonical);
  bool fits = used + 1 + n + 4 + length <= SLOT - HEADER;
  if (fits) {
    uint8_t *entry = block + HEADER + used;
    entry[0] = (uint8_t)n;
    memcpy(entry + 1, canonical, n);
    put32(entry + 1 + n, length);
    if (length) memcpy(entry + 1 + n + 4, data, length);
    used += (uint32_t)(1 + n + 4 + length);
    count++;
    memcpy(block, "D64S", 4);
    put32(block + 4, 1);
    put32(block + 8, store.generation + 1);
    put32(block + 12, used);
    put32(block + 20, count);
    put32(block + 16, crc(block));
  }
  int slot = store.slot == 0 ? 1 : 0;
  bool ok = fits &&
            store.backend.write(store.backend.context, (size_t)slot * SLOT, block, SLOT) &&
            store.backend.read(store.backend.context, (size_t)slot * SLOT, check, SLOT) &&
            !memcmp(block, check, SLOT);
  if (ok) {
    memcpy(store.data, block + HEADER, sizeof(store.data));
    store.slot = slot;
    store.generation++;
    store.used = used;
    store.count = count;
    store.status = SAVE_VALID;
  }
  free(block);
  free(check);
  return ok;
}

// The nth file (from 1) a folder listing shows: the [player] folders entry
// whose name is one of the path's components ("*" any path) lists them.
const char *game_nth_file(void *ctx, const char *path, int n) {
  (void)ctx;
  char lower[256];
  size_t length = strlen(path);
  if (length >= sizeof(lower)) return "";
  for (size_t i = 0; i <= length; i++)
    lower[i] = path[i] == '/' ? '\\' : (char)tolower((unsigned char)path[i]);
  const char *folder = NULL;
  for (unsigned i = 0; i < COUNT(folders) && !folder; i++) {
    const char *key = folders[i][0];
    if (!key) continue;
    if (!strcmp(key, "*")) {
      folder = key;
      break;
    }
    size_t k = strlen(key);
    for (const char *at = strstr(lower, key); at && !folder; at = strstr(at + 1, key))
      if ((at == lower || at[-1] == '\\' || at[-1] == ':') && (!at[k] || at[k] == '\\'))
        folder = key;
  }
  int index = 0;
  for (unsigned i = 0; folder && i < COUNT(folders); i++)
    if (folders[i][0] && !strcmp(folders[i][0], folder) && ++index == n) return folders[i][1];
#ifdef GAME_SAVE_PATHS
  // Enumerate each immediate saved child once, including inferred parents.
  char prefix[NAME];
  char requested[256];
  int written = snprintf(requested, sizeof(requested), "%s%s", path,
      length && (path[length - 1] == '/' || path[length - 1] == '\\' || path[length - 1] == ':') ? "" : "/");
  if (written < 0 || (size_t)written >= sizeof(requested) || !save_name(requested, prefix)) return "";
  size_t plen = strlen(prefix);
  static char child[NAME];
  for (unsigned i = 0; i < store.count; i++) {
    const char *saved = game_save_name(i);
    if (strncmp(saved, prefix, plen) || !saved[plen]) continue;
    size_t size = strcspn(saved + plen, "/");
    bool dir = saved[plen + size] == '/';
    memcpy(child, saved + plen, size);
    child[size] = dir ? '\\' : 0;
    child[size + (dir ? 1 : 0)] = 0;
    bool duplicate = false;
    for (unsigned j = 0; j < i; j++) {
      const char *prior = game_save_name(j);
      if (!strncmp(prior, prefix, plen) && !strncmp(prior + plen, child, size) &&
          (dir ? prior[plen + size] == '/' : !prior[plen + size])) { duplicate = true; break; }
    }
    if (!duplicate && ++index == n) return child;
  }
#endif
  return "";
}

bool game_directory(void *ctx, const char *path, bool create) {
  if (!path || !*path) return false;
  if (path[1] == ':' && path[0] != 'C' && path[0] != 'c' && path[0] != 'D' && path[0] != 'd') return false;
  if (*game_nth_file(ctx, path, 1)) return true;
  size_t n = strlen(path);
  if (path[1] == ':' && (n == 2 || (n == 3 && (path[2] == '/' || path[2] == '\\')))) return true;
#ifdef GAME_SAVE_PATHS
  char requested[256], canonical[NAME];
  int written = snprintf(requested, sizeof(requested), "%s%s", path,
      path[n - 1] == '/' || path[n - 1] == '\\' || path[n - 1] == ':' ? "" : "/");
  if (written < 0 || (size_t)written >= sizeof(requested) || !save_name(requested, canonical)) return false;
  if (find(store.data, store.count, canonical) >= 0) return true;
  return create && game_write_file(requested, "", 0);
#else
  (void)create;
  return false;
#endif
}

const char *game_long_date(void *ctx) {
  (void)ctx;
#ifdef GAME_LONG_DATE
  return GAME_LONG_DATE;
#else
  return "";
#endif
}
