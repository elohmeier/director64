// A font's text metrics at one size, as the ports' layout reads them
// (compiler/src/ports.rs): FreeType's hinted advance of every codepoint in
// 32..=255 (0 where the font has no glyph or the codepoint is not
// measured), the kerning among the measured codepoints, and the size's
// ascent and descent in whole pixels.
//   font-metrics FONT SIZE RANGE...   RANGE is FIRST-LAST in hex (0x20-0x7E)
// Prints {"advances": [...], "kerning": [[first, second, amount], ...],
// "size": SIZE, "ascent": A, "descent": D}.
//
// Measured as libdragon's mkfont did, with the same FreeType
// (third_party/freetype): the size is a nominal pixel height; a font whose
// glyphs render without grey levels at that size is hinted for monochrome.
// Kerning is mkfont's table: each range's glyphs paired among themselves
// and, for a range that is not ASCII, with the first 96 glyphs; in glyph
// order, repeats kept, as pixels * 127 / size.
#include "FreeTypeAmalgam.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool monochrome(FT_Face face) {
  for (int ch = 'a'; ch <= 'f'; ch++) {
    FT_UInt index = FT_Get_Char_Index(face, (FT_ULong)ch);
    if (!index || FT_Load_Glyph(face, index, FT_LOAD_RENDER)) continue;
    FT_Bitmap *bitmap = &face->glyph->bitmap;
    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) continue;
    for (unsigned y = 0; y < bitmap->rows; y++)
      for (unsigned x = 0; x < bitmap->width; x++) {
        unsigned char v = bitmap->buffer[y * bitmap->pitch + x];
        if (v != 0 && v != 0xFF) return false;
      }
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: font-metrics FONT SIZE FIRST-LAST...\n");
    return 2;
  }
  int size = atoi(argv[2]);
  FT_Library library;
  FT_Face face;
  if (FT_Init_FreeType(&library) || FT_New_Face(library, argv[1], 0, &face)) {
    fprintf(stderr, "font-metrics: %s: cannot open font\n", argv[1]);
    return 1;
  }
  FT_Size_RequestRec request = {.type = FT_SIZE_REQUEST_TYPE_NOMINAL, .height = size << 6};
  if (FT_Request_Size(face, &request)) {
    fprintf(stderr, "font-metrics: %s: cannot set size %d\n", argv[1], size);
    return 1;
  }
  int load = FT_LOAD_RENDER | (monochrome(face) ? FT_LOAD_TARGET_MONO : 0);
  // The ranges in glyph order: every codepoint of a range takes a glyph
  // slot, found in the font or not.
  int range_first[64], range_count = 0;
  unsigned codepoints[256], glyph_count = 0;
  for (int i = 3; i < argc && range_count < 64; i++) {
    unsigned first, last;
    if (sscanf(argv[i], "%x-%x", &first, &last) != 2 || first > last || last > 255) {
      fprintf(stderr, "font-metrics: bad range %s\n", argv[i]);
      return 2;
    }
    range_first[range_count++] = (int)glyph_count;
    for (unsigned c = first; c <= last && glyph_count < 256; c++) codepoints[glyph_count++] = c;
  }
  range_first[range_count] = (int)glyph_count;
  FT_UInt index[256] = {0};
  int advance[256] = {0};
  for (unsigned g = 0; g < glyph_count; g++) {
    index[g] = FT_Get_Char_Index(face, codepoints[g]);
    if (index[g] && !FT_Load_Glyph(face, index[g], load))
      advance[codepoints[g]] = (int)(face->glyph->advance.x / 64);
  }
  printf("{\"advances\":[");
  for (int c = 32; c <= 255; c++) printf("%s%d", c > 32 ? "," : "", advance[c]);
  printf("],\"kerning\":[");
  // By glyph pair: the amount, and how often mkfont's loops added it.
  static int amount[256][256];
  static unsigned char repeats[256][256];
  if (FT_HAS_KERNING(face))
    for (int r = 0; r < range_count; r++) {
      int first = range_first[r], n = range_first[r + 1] - first;
      int total = n + (codepoints[first] != 0x20 ? 96 : 0);
      for (int i = 0; i < total; i++) {
        int g1 = i < n ? first + i : i - n;
        if (g1 >= (int)glyph_count || !index[g1]) continue;
        for (int j = 0; j < total; j++) {
          int g2 = j < n ? first + j : j - n;
          if (g2 >= (int)glyph_count || !index[g2]) continue;
          FT_Vector kerning;
          if (FT_Get_Kerning(face, index[g1], index[g2], FT_KERNING_DEFAULT, &kerning) ||
              !(kerning.x >> 6))
            continue;
          amount[g1][g2] = (int)(kerning.x >> 6) * 127 / size;
          repeats[g1][g2]++;
        }
      }
    }
  bool first_pair = true;
  for (unsigned g1 = 0; g1 < glyph_count; g1++)
    for (unsigned g2 = 0; g2 < glyph_count; g2++)
      for (unsigned k = 0; k < repeats[g1][g2]; k++) {
        printf("%s[%u,%u,%d]", first_pair ? "" : ",", codepoints[g1], codepoints[g2],
               amount[g1][g2]);
        first_pair = false;
      }
  printf("],\"size\":%d,\"ascent\":%ld,\"descent\":%ld}\n", size,
         (long)(face->size->metrics.ascender >> 6), (long)-(face->size->metrics.descender >> 6));
  FT_Done_Face(face);
  FT_Done_FreeType(library);
  return 0;
}
