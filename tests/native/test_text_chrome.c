// Classic field presentation is separate from its logical text rectangle.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t cached, uncached;
static const lv_movie_t code = {.name = "CHROME.DXR"};
static const dg_cast_t cast = {.name = "Internal", .file = 1, .cast = 1};
static const dg_text_style_t style = {
    .font_name = "Synthetic", .font_id = 1, .size = 12, .line_height = 14};
static const dg_member_t member = {.id = DG_ID(1, 1, 1),
                                   .cast = 1,
                                   .number = 1,
                                   .type = 3,
                                   .name = "Field",
                                   .text = "",
                                   .width = 40,
                                   .height = 14,
                                   .text_style = &style,
                                   .text_box_present = true,
                                   .text_box_type = 2,
                                   .text_margin = 2,
                                   .text_border = 1,
                                   .text_box_shadow = 2,
                                   .text_word_wrap = false,
                                   .text_box_bg = 0x123456};
static const dg_frame_t frame = {0, 0};
static const uint32_t palette[256] = {0};
static const dg_movie_t movie = {.id = 1,
                                 .code = &code,
                                 .casts = &cast,
                                 .cast_count = 1,
                                 .members = &member,
                                 .member_count = 1,
                                 .frames = &frame,
                                 .frame_count = 1,
                                 .stage_color = 0xabcdef,
                                 .palette = palette};
static int measure(void *ctx, uint32_t id, unsigned width) {
  (void)ctx;
  assert(id == member.id && width == 40);
  return 14;
}
static int char_at(void *ctx, uint32_t id, unsigned channel, int x, int y) {
  (void)ctx;
  assert(id == member.id && channel == 1);
  if (x != 0)
    return -1;
  for (unsigned i = 0; i < director.field_count; i++)
    if (director.field_members[i] == id)
      y += director.field_scroll[i];
  return y >= 0 && y < 14 ? 0 : y >= 14 && y < 28 ? 4 : -1;
}
static int get(const char *name, lv_t owner) {
  int result = lv_integer(&values, lv_get(&values, NULL, name, owner));
  assert(!values.failed);
  return result;
}
static void render(void) {
  wc_render(&cached, &director, 0, 0, false, 0);
  wc_render(&uncached, &director, 0, 0, false, 0);
  assert(!values.failed &&
         !memcmp(cached.pixels, uncached.pixels, sizeof(cached.pixels)));
}
static uint32_t pixel(int x, int y) { return cached.pixels[y * WC_WIDTH + x]; }
int main(void) {
  dg_init(&director, &values,
          (dg_platform_t){.text_height = measure, .text_char_at = char_at},
          NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  director.sprites[1] = (dg_sprite_t){.visible = true,
                                      .value = {.type = 16,
                                                .member = member.id,
                                                .x = 30,
                                                .y = 30,
                                                .width = 40,
                                                .height = 14,
                                                .ink = 0,
                                                .blend = 100}};
  dg_sprite_changed(&director, 1);
  wc_init(&cached, (wc_host_t){0}, 4096);
  wc_init(&uncached, (wc_host_t){0}, 4096);
  wc_cache_enabled(&uncached, false);
  lv_t owner = lv_make(LV_MEMBER, member.id);
  assert(get("margin", owner) == 2 && get("border", owner) == 1 &&
         get("boxDropShadow", owner) == 2);
  assert(!get("wordWrap", owner));
  assert(get("width", owner) == 40 && get("height", owner) == 14);
  render();
  assert(pixel(27, 28) == 0xff000000 && pixel(28, 29) == 0xff563412);
  assert(pixel(30, 30) == 0xff563412 && pixel(74, 47) == 0xff000000);
  assert(pixel(26, 28) == 0xffefcdab && pixel(75, 48) == 0xffefcdab);
  int left, top, right, bottom;
  dg_bounds(&director, 1, &left, &top, &right, &bottom);
  assert(left == 30 && top == 30 && right == 70 && bottom == 44);
  lv_set(&values, NULL, "margin", owner, lv_num(0));
  lv_set(&values, NULL, "border", owner, lv_num(0));
  lv_set(&values, NULL, "boxDropShadow", owner, lv_num(0));
  lv_set(&values, NULL, "wordWrap", owner, lv_num(1));
  lv_set(&values, NULL, "bgColor", owner, lv_make(LV_COLOR, 0x778899));
  assert(get("width", owner) == 40 && get("height", owner) == 14 &&
         get("wordWrap", owner));
  assert(lv_type(lv_get(&values, NULL, "bgColor", owner)) == LV_COLOR);
  render();
  assert(pixel(27, 28) == 0xffefcdab && pixel(74, 47) == 0xffefcdab &&
         pixel(30, 30) == 0xff998877);
  lv_set(&values, NULL, "text", owner, lv_text(&values, "one\rthree", false));
  lv_set(&values, NULL, "rect", owner, lv_rect(&values, 0, 0, 40, 28));
  director.mouse_x = 50;
  director.mouse_y = 31;
  assert(get("mouseLine", 0) == 1);
  director.mouse_y = 45;
  assert(get("mouseLine", 0) == 2);
  lv_set(&values, NULL, "scrollTop", owner, lv_num(14));
  director.mouse_y = 31;
  assert(get("mouseLine", 0) == 2);
  director.mouse_x = 71;
  assert(get("mouseLine", 0) == -1);
  wc_release(&cached);
  wc_release(&uncached);
  puts("Field chrome: source settings, outside gutters/borders/shadow, logical "
       "bounds and cached edits pass");
}
