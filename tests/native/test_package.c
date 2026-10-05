// Package loader contracts on a synthetic package: a valid one loads into
// the structures the runtime reads; wrong ABI, profile or format, every
// truncation, and every single-byte corruption are rejected or load into
// tables the runtime can still index safely (run it under ASan).
#include "package.h"
#include "lingo_bytecode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t buffer[8192];
static size_t used;
static void put8(uint8_t v) { buffer[used++] = v; }
static void put16(uint16_t v) { put8((uint8_t)v); put8((uint8_t)(v >> 8)); }
static void put32(uint32_t v) { put16((uint16_t)v); put16((uint16_t)(v >> 16)); }
static void put(const void *data, size_t n) { memcpy(buffer + used, data, n); used += n; }

static const uint8_t abi[DG_PACKAGE_ABI_BYTES] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
// String pool offsets.
enum { S_EMPTY = 0, S_MOVIE = 1, S_HANDLER = 11, S_CAST = 21, S_KIND = 30, S_ASSET = 42,
       S_SYMBOL = 48, S_GLOBAL = 54 };
static const char strings[] = "\0MOVIE.DXR\0exitframe\0Internal\0MovieScript\0a.fdi\0hello\0gscore";

static size_t build(void) {
  static const uint8_t code[] = {LB_LINE, 0, 3, LB_GLOBAL, 0, 0, LB_TEXT, 0, 0, LB_JUMP, 0, 0,
                                 LB_RETURN_VOID};
  size_t start;
  used = 0;
  // Sections are written after a fixed-size header and table.
  size_t meta = 32 + 12 * 6, strs = meta + 2, code_at = strs + sizeof(strings);
  size_t symb = code_at + sizeof(code), glob = symb + 4 + 4 + 4 + 8, movi = glob + 8;
  put("D64P", 4);
  put16(DG_PACKAGE_FORMAT);
  put16(600);
  put16(0);
  put16(6);
  put(abi, sizeof(abi));
  put32(0);
  const struct { const char *tag; size_t at, n; } table[] = {
      {"META", meta, 2}, {"STRS", strs, sizeof(strings)}, {"CODE", code_at, sizeof(code)},
      {"SYMB", symb, 20}, {"GLOB", glob, 8}, {"MOVI", movi, 0}};
  size_t table_at = used;
  for (unsigned i = 0; i < 6; i++) {
    put(table[i].tag, 4);
    put32((uint32_t)table[i].at);
    put32((uint32_t)table[i].n);
  }
  put("{}", 2);
  put(strings, sizeof(strings));
  put(code, sizeof(code));
  put32(1); put32(1); put32(S_SYMBOL); put32(0); put32(1);  // symbols
  put32(1); put32(S_GLOBAL);                                 // globals
  start = used;
  put32(1);                    // one movie
  put32(S_MOVIE);
  put32(1);                    // one handler
  put32(S_HANDLER); put32(7); put32(S_CAST); put32(S_KIND);
  put32(0); put32(0); put32(0); put32(0);
  put32(0); put32((uint32_t)sizeof(code));
  put32(1); put32(S_SYMBOL); put16(LB_NAME_COUNT); put16(0); // name pool
  put32(0);                    // doubles
  put32(1); put16(0); put16(0); put16(1); // handler index
  put16(1); put16(30);         // id, tempo
  put16(1); put32(S_CAST); put16(1); put16(1);
  put32(1);                    // one member
  put32(1u << 20 | 1u << 16 | 1); put16(1); put16(1); put16(1); put16(8); put16(4);
  put16(0); put16(0); put32(S_EMPTY); put32(S_ASSET); put32(S_EMPTY);
  put32(0); put32(0); put32(0); put32(0); put16(0); put16(0);
  for (unsigned i = 0; i < 6; i++) put8(0);
  put32(0x12345678); put16(0); // member index
  for (unsigned i = 0; i < 256; i++) put32(i);
  put32(1); put32(0); put16(1); // one frame, one delta
  put32(1); put16(6); put16(0x8000); put32(1u << 20 | 1u << 16 | 1); put32(0);
  put16(10); put16(20); put16(8); put16(4);
  for (unsigned i = 0; i < 9; i++) put8((uint8_t)i);
  put16(1); put32(S_HANDLER); put16(1); // one label
  // Patch the MOVI length now that it is known.
  uint32_t length = (uint32_t)(used - start);
  buffer[table_at + 5 * 12 + 8] = (uint8_t)length;
  buffer[table_at + 5 * 12 + 9] = (uint8_t)(length >> 8);
  assert(start == movi);
  return used;
}

static bool load(const uint8_t *data, size_t length, dg_package_t *p, char *error) {
  return dg_package_load(p, data, length, abi, error, 256);
}

int main(void) {
  size_t length = build();
  dg_package_t p;
  char error[256];
  assert(load(buffer, length, &p, error));
  assert(p.movie_count == 1 && p.global_count == 1 && !strcmp(p.globals[0], "gscore"));
  assert(p.symbols.count == 1 && !strcmp(p.symbols.text[0], "hello"));
  const dg_movie_t *m = p.registry[0];
  assert(m->id == 1 && m->tempo == 30 && !strcmp(m->code->name, "MOVIE.DXR"));
  assert(m->code->count == 1 && m->code->handlers[0].member == 7);
  assert(m->code->handlers[0].code_size == 13 && m->code->handlers[0].code[0] == LB_LINE);
  assert(m->member_count == 1 && !strcmp(m->members[0].asset, "a.fdi"));
  assert(m->members[0].width == 8 && m->palette[255] == 255);
  assert(m->frame_count == 1 && m->deltas[0].channel == 6 && m->deltas[0].mask == 0x8000);
  assert(m->deltas[0].value.x == 10 && m->deltas[0].value.trails == 8);
  assert(m->label_count == 1 && !strcmp(m->labels[0].name, "exitframe"));
  dg_package_free(&p);

  // Incompatible packages name why.
  uint8_t copy[sizeof(buffer)];
  memcpy(copy, buffer, length);
  copy[12] ^= 1;
  assert(!load(copy, length, &p, error) && strstr(error, "ABI"));
  memcpy(copy, buffer, length);
  copy[6] = 0x20; // D8
  assert(!load(copy, length, &p, error) && strstr(error, "profile"));
  memcpy(copy, buffer, length);
  copy[4] = DG_PACKAGE_FORMAT + 1;
  assert(!load(copy, length, &p, error) && strstr(error, "format"));
  memcpy(copy, buffer, length);
  copy[0] = 'X';
  assert(!load(copy, length, &p, error));

  // Every truncation is rejected; nothing stays allocated (ASan checks).
  for (size_t n = 0; n < length; n++) {
    uint8_t *prefix = malloc(n ? n : 1);
    memcpy(prefix, buffer, n);
    assert(!load(prefix, n, &p, error));
    assert(!p.registry && !p.blocks && !p.bytes);
    free(prefix);
  }
  // Single-byte corruption past the header either fails a check or loads
  // tables whose every index stays in bounds; neither may crash.
  unsigned rejected = 0, accepted = 0;
  for (size_t at = 12; at < length; at++)
    for (unsigned bit = 0; bit < 8; bit += 3) {
      memcpy(copy, buffer, length);
      copy[at] ^= (uint8_t)(1u << bit);
      if (load(copy, length, &p, error)) {
        accepted++;
        const dg_movie_t *movie = p.registry[0];
        for (unsigned f = 0; f < movie->frame_count; f++)
          assert(movie->frames[f].first + movie->frames[f].count <= 1);
        for (unsigned h = 0; h < movie->code->count; h++)
          assert(movie->code->handlers[h].entry < movie->code->handlers[h].code_size);
        dg_package_free(&p);
      } else {
        rejected++;
      }
    }
  assert(rejected > 100 && accepted > 0);
  printf("package loader: valid load, incompatibility, truncation and corruption passed "
         "(%u rejected, %u safe)\n", rejected, accepted);
  return 0;
}
