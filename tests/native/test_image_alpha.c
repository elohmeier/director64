#include "ink.h"
#include "pack.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}
static void test_pack_keys_and_records(void) {
  // Vectors shared with tests/test_image_pack.py: the packer's keys are the
  // console's.
  assert(dg_image_key("2044807783f1979d6c83bbd5.fdi", 36) == 0xec740a63u);
  assert(dg_image_key("a.fdi", 255) == 0x1b712721u);
  assert(dg_image_key("a.fdi", 0) & 1u);
  const uint8_t header[32] = {'D', '6', '4', 'P', 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1,
                              0, 0, 0, 40, 0, 0, 0, 32, 0, 0, 0, 88, 0, 0, 0, 112};
  dg_pack_header_t parsed;
  assert(dg_pack_header_parse(header, &parsed));
  assert(parsed.index_count == 2 && parsed.scene_count == 1 && parsed.index_offset == 40);
  assert(parsed.scene_table_offset == 32 && parsed.entries_offset == 88 &&
         parsed.blobs_offset == 112);
  uint8_t wrong[32];
  memcpy(wrong, header, 32);
  wrong[7] = 2;
  assert(!dg_pack_header_parse(wrong, &parsed));
  const uint8_t entry[28] = {0xec, 0x74, 0x0a, 0x63, 0, 0, 1, 0, 0, 0, 0, 45,
                             0, 0, 0x1a, 0x40, 0, 0, 0x1a, 0x2c, 0, 3, 0, 2,
                             0x03, 0xe8, 0, 0};
  dg_pack_entry_t e;
  dg_pack_entry_parse(entry, &e);
  assert(e.key == 0xec740a63u && e.offset == 256 && e.compressed == 45);
  assert(e.allocation == 6720 && e.length == 6700 && e.width == 3 && e.height == 2);
  assert(e.share == 1000 && e.flags == 0);
}
int main(void) {
  test_pack_keys_and_records();
  uint32_t alpha_offset;
  assert(dg_fdia_size(3, 3, &alpha_offset) == 65 && alpha_offset == 56);
  assert(dg_fdia_size(640, 480, &alpha_offset) == 921632 && alpha_offset == 614432);
  // Past the texture limit both planes hold the padded tile count, and the
  // tile count already satisfies the eight-byte alignment the linear case
  // rounds up to. ts_st6 (1786x149) is what the train station scrolls in.
  assert(dg_fdia_size(1786, 149, &alpha_offset) == 860192 && alpha_offset == 573472);
  assert(dg_fdia_size(1182, 127, &alpha_offset) == 454688 && alpha_offset == 303136);
  assert(!dg_fdia_size(0, 3, &alpha_offset));
  assert(!dg_fdia_size(65535, 65535, &alpha_offset));
  // FDIC: the palette after the header, the index plane at the next eight
  // byte boundary; CI4 linear rows are ceil(width / 2) bytes, tiles keep the
  // tile-major order of every plane.
  assert(dg_fdic_index_offset(3) == 40 && dg_fdic_index_offset(16) == 64 &&
         dg_fdic_index_offset(256) == 544);
  assert(dg_fdic_size(3, 2, 4, 3) == 44 && dg_fdic_size(3, 2, 8, 17) == 72 + 6);
  // 1056x1 is 33 tiles of 1024 stored pixels.
  assert(dg_fdic_size(1056, 1, 4, 3) == 40 + 33 * 512 && dg_fdic_size(1056, 1, 8, 200) == 432 + 33 * 1024);
  assert(!dg_fdic_size(3, 2, 4, 17) && !dg_fdic_size(3, 2, 8, 257) && !dg_fdic_size(3, 2, 5, 3) &&
         !dg_fdic_size(0, 2, 4, 3));
  {
    // The packer's 3x2 example: palette ffff 0001 fffe, rows 01 20 / 10 00.
    uint8_t image[44] = {'F', 'D', 'I', 'C', 0, 0, 0, 44, 0, 3, 0, 2, 0, 0, 0, 0, 0, 0, 4, 0, 0, 3};
    const uint8_t palette[] = {0xff, 0xff, 0x00, 0x01, 0xff, 0xfe};
    const uint8_t plane[] = {0x01, 0x20, 0x10, 0x00};
    for (unsigned i = 0; i < 6; i++) image[32 + i] = palette[i];
    for (unsigned i = 0; i < 4; i++) image[40 + i] = plane[i];
    unsigned bits, colors;
    uint32_t index_offset;
    assert(dg_fdic_valid(image, 44, 3, 2, &bits, &colors, &index_offset));
    assert(bits == 4 && colors == 3 && index_offset == 40);
    assert(!dg_fdic_valid(image, 43, 3, 2, &bits, &colors, &index_offset));
    assert(!dg_fdic_valid(image, 44, 4, 2, &bits, &colors, &index_offset));
    assert(dg_fdic_texel(image, 4, 40, 3, 2, 0, 0) == 0xffff);
    assert(dg_fdic_texel(image, 4, 40, 3, 2, 1, 0) == 0x0001);
    assert(dg_fdic_texel(image, 4, 40, 3, 2, 2, 0) == 0xfffe);
    assert(dg_fdic_texel(image, 4, 40, 3, 2, 0, 1) == 0x0001);
    assert(dg_fdic_texel(image, 4, 40, 3, 2, 2, 1) == 0xffff);
    // The matte reads the alpha bit through the palette: white opaque, white clear.
    assert((dg_fdic_texel(image, 4, 40, 3, 2, 0, 0) & 1) && !(dg_fdic_texel(image, 4, 40, 3, 2, 2, 0) & 1));
  }
  {
    // CI8, and a tile-packed CI4 plane: pixel (33, 0) of a 1056-wide image
    // is stored pixel 1025 (tile 1, column 1), the low nibble of byte 512.
    enum { WIDE = 40 + 33 * 512 };
    static uint8_t wide[WIDE];
    memcpy(wide, "FDIC", 4);
    wide[4] = 0; wide[5] = 0; wide[6] = (uint8_t)(WIDE >> 8); wide[7] = (uint8_t)WIDE;
    wide[8] = 1056 >> 8; wide[9] = 1056 & 255; wide[11] = 1; wide[18] = 4; wide[21] = 3;
    wide[32] = 0x11; wide[33] = 0x11; wide[34] = 0x22; wide[35] = 0x22; wide[36] = 0x33; wide[37] = 0x33;
    wide[40 + 512] = 0x12;
    unsigned bits, colors;
    uint32_t index_offset;
    assert(dg_fdic_valid(wide, WIDE, 1056, 1, &bits, &colors, &index_offset) && index_offset == 40);
    assert(dg_fdic_texel(wide, 4, 40, 1056, 1, 32, 0) == 0x2222);
    assert(dg_fdic_texel(wide, 4, 40, 1056, 1, 33, 0) == 0x3333);
    assert(dg_fdic_texel(wide, 4, 40, 1056, 1, 0, 0) == 0x1111);
    uint8_t ci8[72 + 6] = {'F', 'D', 'I', 'C', 0, 0, 0, 78, 0, 3, 0, 2, 0, 0, 0, 0, 0, 0, 8, 0, 0, 17};
    for (unsigned i = 0; i < 17; i++) { ci8[32 + i * 2] = (uint8_t)i; ci8[33 + i * 2] = 1; }
    for (unsigned i = 0; i < 6; i++) ci8[72 + i] = (uint8_t)(16 - i);
    assert(dg_fdic_valid(ci8, 78, 3, 2, &bits, &colors, &index_offset) && bits == 8 && index_offset == 72);
    assert(dg_fdic_texel(ci8, 8, 72, 3, 2, 0, 0) == 0x1001 && dg_fdic_texel(ci8, 8, 72, 3, 2, 2, 1) == 0x0b01);
  }
  uint8_t header[32] = {'F', 'D', 'I', 'A'};
  be32(header + 4, 65);
  header[9] = header[11] = 3;
  header[16] = 1;
  header[17] = 127;
  be32(header + 20, 56);
  assert(dg_fdia_valid(header, 65, 3, 3, &alpha_offset) && alpha_offset == 56);
  assert(!dg_fdia_valid(header, 64, 3, 3, &alpha_offset));
  assert(!dg_fdia_valid(header, 65, 4, 3, &alpha_offset));
  be32(header + 20, 50);
  assert(!dg_fdia_valid(header, 65, 3, 3, &alpha_offset));
  be32(header + 20, 56);
  header[16] = 0;
  assert(!dg_fdia_valid(header, 65, 3, 3, &alpha_offset));
  for (unsigned a = 0; a < 256; a++) {
    assert(dg_bitmap_alpha16(0xffff, (uint8_t)a, 0) == a);
    assert(dg_bitmap_alpha16(0x1000, (uint8_t)a, 32) == a);
    assert(dg_bitmap_alpha16(0xfffe, (uint8_t)a, 36) == 0);
    assert(dg_bitmap_alpha16(0x1000, (uint8_t)a, 36) == a);
  }
  assert(dg_bitmap_ink32(0xffffff7fu, 0, true) == 0xffffff7fu);
  assert(dg_bitmap_ink32(0xff000001u, 32, true) == 0xff000001u);
  assert(dg_bitmap_ink32(0xffffff7fu, 0, false) == 0xffffffffu);
  assert(dg_bitmap_ink32(0xffffff7fu, 36, true) == 0xffffff00u);
  assert(dg_dark_rgba32(0xff000000u, 0xffff, 100) == 0xffff);
  assert(dg_dark_rgba32(0xff000080u, 0xffff, 100) == 0xfbdf);
  assert(dg_dark_rgba32(0x00ff00ffu, 0xf801, 100) == 1);
  assert(dg_dark_rgba32(0x00ff00ffu, 0xf801, 50) == 0x7bc1);
  // Separate-plane coverage ignores bit0 of RGB5551, including soft alpha1.
  assert(dg_dark_alpha16(0xf800, 0, 0xffff, 100) == 0xffff);
  assert(dg_dark_alpha16(0xf800, 1, 0xffff, 100) == 0xffff);
  assert(dg_dark_alpha16(0xf800, 128, 0xffff, 100) == 0xfbdf);
  assert(dg_dark_alpha16(0xf800, 255, 0xffff, 100) == 0xf801);
  assert(dg_dark_alpha16(0x07c0, 255, 0xf801, 100) == 1);
  assert(dg_dark_alpha16(0x07c0, 255, 0xf801, 50) == 0x7bc1);
  puts("Source alpha: FDIA layout, all256 alpha levels, copy/transparent/Darken PASS");
}
