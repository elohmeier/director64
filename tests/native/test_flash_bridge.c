#include "director.h"
#include "flash_bridge.h"
#include "flash_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t d;
static lv_runtime_t r;
static const dg_film_layer_t child_layer = {
    .channel = 1,
    .clip_id = 7,
    .name = "MixedCase",
    .placement_generation = 1,
    .affine = true,
    .matrix = {65536, 0, 0, 65536, 0, 0}};
static const dg_film_frame_t frames[] = {{1, &child_layer}},
                             empty[] = {{0, NULL}};
static const dg_flash_clip_t clips[] = {
    {.id = 0, .frame_count = 1}, {.id = 7, .frame_count = 1, .frames = empty}};
static const dg_member_t member = {.id = DG_ID(1, 1, 1),
                                   .number = 1,
                                   .cast = 1,
                                   .type = 2,
                                   .source_xtra = 1,
                                   .film_count = 1,
                                   .film_frames = frames,
                                   .flash_version = 7,
                                   .flash_clip_count = 2,
                                   .flash_clips = clips};
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static const dg_movie_t movie = {.id = 1,
                                 .cast_count = 1,
                                 .casts = &cast,
                                 .member_count = 1,
                                 .members = &member};
static unsigned used;
static uint8_t code[256];
static void byte(unsigned x) {
  assert(used < sizeof(code));
  code[used++] = (uint8_t)x;
}
static void word(unsigned x) {
  byte(x);
  byte(x >> 8);
}
static void string(const char *s) {
  do {
    byte((unsigned char)*s);
  } while (*s++);
}
static void push_name(const char *s) {
  byte(0x96);
  word((unsigned)strlen(s) + 2);
  byte(0);
  string(s);
  byte(0x1c);
}
static void define(const char *name, unsigned count, const char **parameters,
                   const char *left, const char *right) {
  byte(0x9b);
  unsigned size = used;
  word(0);
  unsigned start = used;
  string(name);
  word(count);
  for (unsigned i = 0; i < count; i++)
    string(parameters[i]);
  unsigned body_size = used;
  word(0);
  unsigned body = used;
  push_name(left);
  push_name(right);
  byte(0x47);
  byte(0x3e);
  unsigned n = used - body;
  code[body_size] = (uint8_t)n;
  code[body_size + 1] = (uint8_t)(n >> 8);
  n = body - start;
  code[size] = (uint8_t)n;
  code[size + 1] = (uint8_t)(n >> 8);
}
static lv_t text(const char *s) { return lv_text(&r, s, false); }
static lv_t call(const char *name, unsigned argc, const lv_t *args) {
  lv_t out = 0;
  bool yield = false;
  assert(r.services.call(&r, name, argc, args, &out, &yield));
  if (r.failed)
    fprintf(stderr, "%s: %s\n", name, r.error);
  assert(!r.failed && !yield);
  return out;
}
int main(void) {
  dg_init(&d, &r, (dg_platform_t){0}, NULL, NULL, 0, 1);
  d.movie = &movie;
  d.loaded[0] = &movie;
  d.loaded_count = 1;
  d.sprites[1] = (dg_sprite_t){.value = {.type = 16, .member = member.id},
                               .visible = true};
  dg_flash_sync(&d);
  assert(dg_flash_service(&d, 100000));
  dg_flash_instance_t *root = dg_flash_root(&d, 1);
  dg_avm1_t *vm = dg_flash_vm(root);
  lv_t object = dg_flash_object(root), sprite = lv_make(LV_SPRITE, 1);
  lv_t child = lv_get(&r, NULL, "mixedcase", sprite);
  assert(child == dg_flash_object(dg_flash_child(root, 1)));
  lv_set(&r, NULL, "_x", child, lv_num(17));
  assert(lv_numeric(dg_avm1_get(vm, child, "_x")) == 17);
  dg_avm1_set(vm, object, "truth", dg_avm1_boolean(true));
  assert(lv_numeric(lv_get(&r, NULL, "truth", sprite)) == 1);
  dg_avm1_set(vm, object, "empty", dg_avm1_null());
  assert(lv_get(&r, NULL, "empty", sprite) == 0);
  lv_t input[] = {sprite, text("answer"), lv_num(42)};
  call("setVariable", 3, input);
  lv_t get[] = {sprite, text("answer")};
  assert(!strcmp(lv_cstr(&r, call("getVariable", 2, get)), "42"));
  lv_t literal[] = {sprite, text("answer"), lv_num(0)};
  assert(lv_numeric(call("getVariable", 3, literal)) == 42);
  lv_t fields[] = {text("Value"), lv_num(7)};
  lv_t bag = lv_list(&r, 2, fields, true);
  lv_t setbag[] = {sprite, text("Options"), bag};
  call("setVariable", 3, setbag);
  lv_t getbag[] = {sprite, text("Options"), lv_num(0)};
  lv_t options = call("getVariable", 3, getbag);
  assert(dg_avm1_owns(vm, options) &&
         lv_numeric(lv_get(&r, NULL, "value", options)) == 7);
  const char *params[] = {"first", "second"};
  define("AddNumbers", 2, params, "first", "second");
  const char *watch_params[] = {"property", "oldValue", "newValue", "userData"};
  define("WatchValue", 4, watch_params, "newValue", "userData");
  byte(0);
  assert(dg_avm1_start(vm, object, code, used));
  dg_avm1_status_t status=dg_avm1_run(vm,10000);
  if(status!=DG_AVM1_IDLE)fprintf(stderr,"bridge bytecode: %s\n",r.error);
  assert(status==DG_AVM1_IDLE);
  lv_t sum_args[] = {sprite, lv_num(5), lv_num(8)};
  assert(lv_numeric(call("addnumbers", 3, sum_args)) == 13);
  lv_t watch_args[] = {text("Value"), dg_avm1_get(vm, object, "WatchValue"),
                       lv_num(10)},
       out = 0;
  assert(dg_avm1_invoke(vm, dg_avm1_get(vm, options, "watch"), options, 3,
                        watch_args, 10000, &out));
  lv_set(&r, NULL, "value", options, lv_num(21));
  assert(!r.failed);
  assert(lv_numeric(dg_avm1_get(vm, options, "Value")) == 31);
  // String-key indexing takes the same pre-mutation watch path as dot access.
  lv_index_set(&r, options, text("value"), lv_num(30));
  assert(!r.failed);
  assert(lv_numeric(dg_avm1_get(vm, options, "Value")) == 40);
  assert(dg_flash_lingo_url(&d, "lingo:sound(2).volume = 129"));
  assert(!r.failed);
  assert(d.channel_volume[1] == 129);
  assert(!dg_flash_lingo_url(&d, "https://example.invalid/"));
  dg_flash_dispose(&d);
  assert(!r.external_roots);
  puts("Lingo/AVM1 object, method, watched-property and sound URL bridge: OK");
}
