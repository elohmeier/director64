// A nonintegrally prescaled source must retain the authored-coordinate floor
// before mapping into its mask. Height20 avoids the <=16x16 cursor exception.
// This executes the actual 800 -> 640 path.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static wc_t compositor;
static dg_runtime_t director;
static lv_runtime_t values;
static const uint32_t palette[256] = {0xffffff, 0};
static const lv_movie_t code = {.name = "MASK-PHASE.DXR"};
static const dg_cast_t cast = {.name = "Internal", .file = 1, .cast = 1};
static const dg_frame_t score[] = {{0, 0}};
#define ID(n) DG_ID(1, 1, n)
static const dg_member_t members[] = {
    {.id = ID(1),
     .number = 1,
     .cast = 1,
     .type = 1,
     .width = 3,
     .height = 20,
     .asset = "source.fdi"},
    {.id = ID(2),
     .number = 2,
     .cast = 1,
     .type = 1,
     .width = 3,
     .height = 20,
     .reg_y = 1,
     .asset = "mask.fdi"},
};
static const dg_movie_t movie = {.id = 1,
                                 .code = &code,
                                 .casts = &cast,
                                 .cast_count = 1,
                                 .members = members,
                                 .member_count = 2,
                                 .frames = score,
                                 .frame_count = 1,
                                 .palette = palette,
                                 .stage_color = 0x0000ff};
static uint32_t read_asset(void *ctx, const char *name, uint8_t *out,
                           uint32_t capacity) {
  (void)ctx;
  if (!out)
    return 160;
  assert(capacity >= 160);
  memset(out, 0, 160);
  memcpy(out, "FDI2", 4);
  out[7] = 160;
  out[9] = 2;
  out[11] = 16;
  out[16] = 1;
  out[19] = 32;
  bool mask = !strcmp(name, "mask.fdi");
  for (unsigned y = 0; y < 16; y++)
    for (unsigned x = 0; x < 2; x++) {
      uint8_t *p = out + 32 + (y * 2 + x) * 4;
      p[0] = mask ? (x ? 255 : 0) : 255;
      p[1] = p[2] = mask ? p[0] : 0;
      p[3] = 255;
    }
  return 160;
}
int main(void) {
  wc_init(&compositor, (wc_host_t){.read = read_asset}, 4096);
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  director.sprites[1] = (dg_sprite_t){.value = {.member = ID(1),
                                                .type = 16,
                                                .width = 3,
                                                .height = 20,
                                                .stretch = 1,
                                                .ink = 9,
                                                .blend = 100,
                                                .fore = 1},
                                      .visible = true,
                                      .stretch = true};
  dg_sprite_changed(&director, 1);
  wc_render(&compositor, &director, 0, 0, false, 0);
  if (values.failed)
    fprintf(stderr, "%s\n", values.error);
  assert(!values.failed);
  // x1 maps to authored1 then mask0. A combined division picks white mask1.
  assert(compositor.scene[0] == 0xff0000ff);
  assert(compositor.scene[1] == 0xff0000ff);
  wc_release(&compositor);
  puts("prescaled mask retains two-stage authored sampling phase");
}
