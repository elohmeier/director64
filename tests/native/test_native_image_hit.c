#define _POSIX_C_SOURCE 200809L
#include "image.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static lv_runtime_t runtime;
static char images[256];
static const uint8_t alpha[] = {0, 1, 127, 128, 254, 255};
static dg_member_t member = {.width = 3, .height = 2, .asset = "fixture.fdi"};

static void be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}
static size_t encoded(uint8_t data[64], const char *magic) {
  // Actual 3x2 images: FDI1 has 12 RGB5551 bytes, FDI2 has 24 RGBA
  // bytes, and FDIA has 12 RGB5551 bytes, four padding bytes, then six A8.
  bool planes = !strcmp(magic, "FDIA"), rgba = !strcmp(magic, "FDI2");
  size_t length = planes ? 54 : rgba ? 56 : 44;
  memset(data, 0, 64);
  memcpy(data, magic, 4);
  be32(data + 4, (uint32_t)length);
  data[9] = 3;
  data[11] = 2;
  data[16] = 1;
  data[17] = 127; // Matte input uses nonzero alpha, not this threshold.
  if (planes) {
    be32(data + 20, 48);
    memset(data + 44, 255, 4);
  }
  for (unsigned i = 0; i < 6; i++) {
    if (rgba) {
      memset(data + 32 + i * 4, 255, 3);
      data[35 + i * 4] = alpha[i];
    } else {
      data[32 + i * 2] = 255;
      // In FDIA deliberately disagree with the alpha plane: RGB's low bit
      // must not control coverage. Opaque white is still a matte hit.
      data[33 + i * 2] = (uint8_t)(254 | (planes ? i == 0 : alpha[i] != 0));
      if (planes) data[48 + i] = alpha[i];
    }
  }
  return length;
}
static void write_image(const uint8_t *data, size_t length) {
  char path[512];
  int n = snprintf(path, sizeof(path), "%s/%s", images, member.asset);
  assert(n > 0 && (size_t)n < sizeof(path));
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fwrite(data, 1, length, file) == length);
  assert(!fclose(file));
}
static void reset_error(void) {
  runtime.failed = false;
  runtime.error[0] = 0;
}
static void valid_hit(unsigned ink, int x, int y, bool expected) {
  reset_error();
  assert(native_image_hit(&runtime, &member, ink, x, y) == expected);
  assert(!runtime.failed);
}
static void invalid_image(const uint8_t *data, size_t length) {
  write_image(data, length);
  reset_error();
  assert(!native_image_hit(&runtime, &member, 8, 1, 0));
  assert(runtime.failed && strstr(runtime.error, "invalid native hit image"));
  reset_error();
  dg_cursor_bitmap_t cursor;
  assert(!native_cursor_bitmap(&runtime,&member,NULL,&cursor));
  assert(runtime.failed && strstr(runtime.error,"invalid native cursor image"));
}
int main(void) {
  char workspace[] = "/tmp/director64-image-XXXXXX", director[256];
  const char *old = getenv("DIRECTOR64_WORK_DIR");
  char *saved = old ? strdup(old) : NULL;
  assert(!old || saved);
  assert(mkdtemp(workspace));
  assert(snprintf(director, sizeof(director), "%s/director", workspace) > 0);
  assert(snprintf(images, sizeof(images), "%s/director/images", workspace) > 0);
  assert(!mkdir(director, 0700) && !mkdir(images, 0700));
  assert(!setenv("DIRECTOR64_WORK_DIR", workspace, 1));
  lv_init(&runtime, (lv_services_t){0}, NULL, NULL, 0, 1);

  const char *formats[] = {"FDI1", "FDI2", "FDIA"};
  uint8_t data[64], damaged[64];
  for (unsigned format = 0; format < 3; format++) {
    size_t length = encoded(data, formats[format]);
    write_image(data, length);
    for (unsigned i = 0; i < 6; i++)
      valid_hit(8, (int)(i % 3), (int)(i / 3), alpha[i] != 0);
    valid_hit(8, -1, 0, false);
    valid_hit(8, 3, 0, false);
    valid_hit(8, 0, -1, false);
    valid_hit(8, 0, 2, false);
    valid_hit(8, INT_MIN, INT_MAX, false);
    valid_hit(8, INT_MAX, INT_MIN, false);
    // Background-transparent ink's white exclusion is a rendering rule;
    // its input remains rectangular, including fully transparent pixels.
    valid_hit(36, 0, 0, true);
    valid_hit(0, 0, 0, true);
    dg_cursor_bitmap_t cursor;
    uint16_t cursor_pixels[256];
    assert(native_cursor_bitmap(&runtime,&member,NULL,&cursor));
    dg_cursor_pixels(&cursor,cursor_pixels);
    // Cursor coverage ignores FDI1 alpha, FDIA coverage and FDI2 alpha alike.
    for(unsigned y=0;y<16;y++) for(unsigned x=0;x<16;x++)
      assert(cursor_pixels[y*16+x]==(x<3 && y<2 ? 0xffff : 0));
    assert(native_cursor_bitmap(&runtime,&member,&member,&cursor));
    dg_cursor_pixels(&cursor,cursor_pixels);
    for(unsigned i=0;i<256;i++)assert(!cursor_pixels[i]); // White mask is clear.

    invalid_image(data, length - 1);
    invalid_image(data, length + 1);
    memcpy(damaged, data, sizeof(data));
    be32(damaged + 4, (uint32_t)length + 1);
    invalid_image(damaged, length);
    memcpy(damaged, data, sizeof(data));
    damaged[9] = 4;
    invalid_image(damaged, length);
    memcpy(damaged, data, sizeof(data));
    damaged[11] = 3;
    invalid_image(damaged, length);
  }
  size_t length = encoded(data, "FDIA");
  for (size_t truncated = 0; truncated < 32; truncated++)
    invalid_image(data, truncated);
  const uint32_t offsets[] = {0, 31, 32, 44, 47, 49, UINT32_MAX - 1, UINT32_MAX};
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); i++) {
    memcpy(damaged, data, sizeof(data));
    be32(damaged + 20, offsets[i]);
    invalid_image(damaged, length);
  }
  data[16] = 0;
  invalid_image(data, length);
  data[0] = 'X';
  invalid_image(data, length);
  length = encoded(data, "FDI2");
  data[8] = data[9] = data[10] = data[11] = 255;
  member.width = member.height = UINT16_MAX;
  invalid_image(data, length);

  char path[512];
  assert(snprintf(path, sizeof(path), "%s/%s", images, member.asset) > 0);
  assert(!unlink(path));
  // No image I/O is needed when Director's hit rule is rectangular.
  valid_hit(36, 0, 0, true);
  reset_error();
  assert(!native_image_hit(&runtime, &member, 8, 0, 0) && runtime.failed);
  assert(!rmdir(images) && !rmdir(director) && !rmdir(workspace));
  if (saved) {
    assert(!setenv("DIRECTOR64_WORK_DIR", saved, 1));
    free(saved);
  } else {
    assert(!unsetenv("DIRECTOR64_WORK_DIR"));
  }
  puts("Native matte files: FDI1/FDI2/FDIA, alpha boundaries and invalid extents PASS");
}
