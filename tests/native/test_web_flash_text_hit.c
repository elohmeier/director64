// Text used as a Flash geometric mask must hit glyph coverage, not its box.
#include "compositor.h"
#include "flash_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t values;
static wc_t compositor;
static unsigned rasters;
static const dg_text_style_t style = {
    .font_name = "SourceFree", .font_id = 3, .size = 10, .ascent = 8};
static const dg_flash_field_t field = {
    .id = 7,
    .name = "label",
    .text = "X",
    .width = 10,
    .height = 10,
    .style = &style,
    .embed_fonts = true,
    .local_bounds = {0, 0, 10 * 65536, 10 * 65536}};
static const dg_film_layer_t layer = {
    .channel = 1, .field_id = 7, .name = "label"};
static const dg_film_frame_t frame = {1, &layer};
static const dg_flash_clip_t clip = {.id = 0, .frame_count = 1};
static const dg_member_t member = {.id = DG_ID(1, 1, 1),
                                   .number = 1,
                                   .cast = 1,
                                   .type = 2,
                                   .source_xtra = 1,
                                   .width = 10,
                                   .height = 10,
                                   .film_count = 1,
                                   .film_frames = &frame,
                                   .flash_version = 8,
                                   .flash_clip_count = 1,
                                   .flash_clips = &clip,
                                   .flash_field_count = 1,
                                   .flash_fields = &field};
static const dg_movie_t movie = {
    .id = 1, .member_count = 1, .members = &member};
static bool raster(void *context, const char *text, int width, int height,
                   const wc_text_style_t *text_style, uint8_t *coverage) {
  (void)context;
  assert(!strcmp(text, "X") && text_style->font == 3 && width > 2 &&
         height > 2);
  coverage[0] = 255;
  coverage[2 * width + 2] = 128;
  rasters++;
  return true;
}
int main(void) {
  dg_init(&d, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  d.movie = &movie;
  d.loaded[d.loaded_count++] = &movie;
  d.sprites[1] = (dg_sprite_t){.visible = true,
                               .value = {.type = 16, .member = member.id}};
  dg_sprite_changed(&d, 1);
  dg_flash_sync(&d);
  assert(dg_flash_service(&d, 100000));
  dg_flash_instance_t *instance = dg_flash_child(dg_flash_root(&d, 1), 1);
  assert(instance);
  wc_init(&compositor, (wc_host_t){.text = raster}, 4096);
  for (unsigned n = 0; n < WC_WIDTH * WC_HEIGHT; n++)
    compositor.pixels[n] = 0x12345678;
#if defined(DIRECTOR64_NATIVE_STAGE) && DIRECTOR64_NATIVE_STAGE
  const float scale = 1;
#else
  const float scale = 0.8f;
#endif
  // Rendering adds the Flash field's two-pixel gutter before prescaling.
  const float start = (int)(2 * scale);
  assert(wc_flash_field_hit(&compositor, &d, instance, (start + .1f) / scale,
                            (start + .1f) / scale));
  assert(wc_flash_field_hit(&compositor, &d, instance, (start + 2.1f) / scale,
                            (start + 2.1f) / scale));
  assert(!wc_flash_field_hit(&compositor, &d, instance, (start + 1.1f) / scale,
                             (start + 1.1f) / scale));
  assert(!wc_flash_field_hit(&compositor, &d, instance, 0, 0));
  assert(rasters > 0 && !values.failed);
  for (unsigned n = 0; n < WC_WIDTH * WC_HEIGHT; n++)
    assert(compositor.pixels[n] == 0x12345678);
  wc_release(&compositor);
  dg_flash_dispose(&d);
  puts("Flash embedded text mask hit samples glyph coverage and preserves the "
       "stage");
}
