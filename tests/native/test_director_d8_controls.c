#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Channel::setPosition in ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc
// and the recovered NB fly / UPPSCRIP drag handlers constrain registration
// points. These host contracts do not establish original-projector parity.
static dg_runtime_t director;
static lv_runtime_t values;
static const lv_movie_t code = {"CONTROLS.DXR", 0, NULL,NULL, NULL, NULL, NULL, 0};
static const dg_member_t members[] = {
    {.id = 0x110001, .type = 1, .width = 100, .height = 80,
     .reg_x = 20, .reg_y = 10},
    {.id = 0x110002, .type = 1, .width = 20, .height = 20,
     .reg_x = 10, .reg_y = 10},
};
static const dg_movie_t movie = {
    .code = &code, .id = 1, .tempo = 30, .members = members, .member_count = 2};
static lv_t sprite(unsigned i) { return lv_make(LV_SPRITE, (int32_t)i); }
static void set(unsigned i, const char *name, lv_t value) {
  lv_set(&values, NULL, name, sprite(i), value);
  assert(!values.failed);
}
static void position(int x, int y) {
  assert(director.sprites[800].value.x == x);
  assert(director.sprites[800].value.y == y);
}
static bool key(lv_t value) {
  lv_t result = {0}; bool yield = false;
  assert(values.services.call(&values, "keypressed", 1, &value, &result, &yield));
  assert(!values.failed && !yield && lv_type(result) == LV_NUMBER);
  return lv_numeric(result) != 0;
}
static lv_flow_t keyboard_handler(lv_runtime_t *r, lv_frame_t *f) {
  unsigned index = !strcmp(f->handler->name, "keydown") ? 0 : 1;
  r->globals[index] = lv_num(lv_numeric(r->globals[index]) + 1);
  r->globals[2] = lv_get(r, f, "key", (lv_t){0});
  r->globals[3] = lv_get(r, f, "keycode", (lv_t){0});
  return LV_RETURN;
}
static void keyboard_events(void) {
  static const char *const globals[] = {"downs", "ups", "key", "code"};
  static const lv_handler_t handlers[] = {
    {.name="keydown",.kind="MovieScript",.step=keyboard_handler},
    {.name="keyup",.kind="MovieScript",.step=keyboard_handler},
  };
  static const lv_movie_t keyboard_code = {"KEYS.DXR",2,handlers,NULL, NULL, NULL, NULL, 0};
  static const dg_movie_t keyboard_movie = {.code=&keyboard_code,.id=1,.tempo=999};
  dg_init(&director, &values, (dg_platform_t){0}, NULL, globals, 4, 42);
  assert(dg_enter(&director, &keyboard_movie, 1, NULL, 0));
  dg_key(&director, 0, 'a', true);
  dg_key(&director, 0, 'a', true); // Polling a held pad must not repeat keyDown.
  assert(dg_tick(&director, 0, 0, false, DG_SERVICE_BUDGET));
  assert(lv_numeric(values.globals[0]) == 1 && lv_numeric(values.globals[3]) == 0);
  assert(!strcmp(lv_cstr(&values, values.globals[2]), "a"));
  dg_key(&director, 0, 0, false);
  assert(dg_tick(&director, 0, 0, false, DG_SERVICE_BUDGET));
  assert(lv_numeric(values.globals[1]) == 1);
  assert(!strcmp(lv_cstr(&values, values.globals[2]), "a"));
  assert(!*lv_cstr(&values, lv_get(&values,NULL,"keypressed",(lv_t){0})));
  dg_key(&director, 123, 28, true);
  dg_key(&director, 124, 29, true);
  for (unsigned i=0;i<2;i++) assert(dg_tick(&director,0,0,false,DG_SERVICE_BUDGET));
  assert(lv_numeric(values.globals[0]) == 3 && lv_numeric(values.globals[3]) == 124);
  assert(key(lv_num(123)) && key(lv_num(124)));
}
static void method_cache_reload(void) {
  static const dg_cast_t casts[]={{"Internal",1,1}};
  static const lv_handler_t first_handlers[]={{.name="first",.member=1,.cast="Internal"}};
  static const lv_handler_t second_handlers[]={{.name="second",.member=1,.cast="Internal"}};
  static const lv_movie_t first_code={"FIRST.DXR",1,first_handlers,NULL,NULL,NULL,NULL,0}, second_code={"SECOND.DXR",1,second_handlers,NULL,NULL,NULL,NULL,0};
  static const dg_movie_t first={.code=&first_code,.id=1,.casts=casts,.cast_count=1}, second={.code=&second_code,.id=1,.casts=casts,.cast_count=1};
  const lv_movie_t *owner=NULL;
  lv_t script=lv_make(LV_SCRIPT, 0x110001);
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,42);
  assert(dg_enter(&director,&first,1,NULL,0));
  for(unsigned n=0;n<3;n++) {
    assert(values.services.resolve(&values,script,"FIRST",&owner)==first_handlers);
    assert(owner==&first_code);
    assert(!values.services.resolve(&values,script,"second",&owner));
  }
  assert(dg_enter(&director,&second,1,NULL,0));
  assert(!values.services.resolve(&values,script,"FIRST",&owner));
  assert(values.services.resolve(&values,script,"second",&owner)==second_handlers);
  assert(owner==&second_code && !values.failed);
}
int main(void) {
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 42);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  set(1, "member", lv_make(LV_MEMBER, 0x110001));
  set(1, "loc", lv_point(&values, 120, 110)); // bounds 100,100,200,180
  set(800, "member", lv_make(LV_MEMBER, 0x110002));
  set(800, "constraint", lv_num(1));
  assert(lv_numeric(lv_get(&values, NULL, "constraint", sprite(800))) == 1);
  set(800, "loc", lv_point(&values, -100, 400));
  position(100, 180); // sprite extents may extend outside the constraint
  set(800, "loch", lv_num(500)); position(200, 180);
  set(800, "locv", lv_num(-1)); position(200, 100);
  set(800, "loc", lv_point(&values, 140, 130)); position(140, 130);
  set(1, "loch", lv_num(170)); // live bounds now 150..250
  set(800, "loc", lv_point(&values, 50, 130)); position(150, 130);
  set(800, "constraint", lv_num(0));
  set(800, "loc", lv_point(&values, -10, 500)); position(-10, 500);
  set(800, "constraint", lv_num(DG_SPRITES));
  set(800, "loc", lv_point(&values, -20, 510)); position(-20, 510);
  set(800, "constraint", lv_make(LV_MEMBER, 0x110001));
  assert(lv_numeric(lv_get(&values, NULL, "constraint", sprite(800))) == 1);
  set(800, "loc", lv_point(&values, 180, 130));
  set(800, "moveablesprite", lv_num(1));
  assert(dg_tick(&director, 180, 130, true, DG_SERVICE_BUDGET));
  assert(director.drag_sprite == 800);
  assert(dg_tick(&director, 500, -100, true, DG_SERVICE_BUDGET));
  position(250, 100);
  assert(dg_tick(&director, 0, 500, false, DG_SERVICE_BUDGET));
  position(150, 180);
  assert(!director.drag_sprite && director.stage_dirty[800]);
  // ScummVM setBbox is a size/rectangle assignment, not setPosition.
  set(800, "rect", lv_rect(&values, 0, 0, 20, 20)); position(10, 10);
  set(800, "locv", lv_num(120)); position(150, 120);
  set(800, "constraint", lv_num(800));
  set(800, "loc", lv_point(&values, 1000, 1000)); position(160, 130);

  assert(!key(lv_num(126)) && !key(lv_text(&values, "f", false)));
  dg_key(&director, 126, 0, true);
  dg_key(&director, 3, 'f', true);
  assert(key(lv_num(126)) && key(lv_num(3)));
  assert(key(lv_text(&values, "F", false)));
  assert(!key(lv_num(125)) && !key(lv_num(128)) && !key(lv_num(-1)));
  assert(!key(lv_text(&values, "", false)) && !key(lv_text(&values, "ff", false)));
  dg_key(&director, 126, 0, false);
  assert(!key(lv_num(126)) && key(lv_text(&values, "f", false)));
  dg_key(&director, 3, 'f', false);
  assert(!key(lv_text(&values, "f", false)));
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  assert(lv_numeric(lv_get(&values, NULL, "constraint", sprite(800))) == 0);
  keyboard_events();
  method_cache_reload();
  puts("D8 controls: constraints, live bounds, drag and held keys PASS");
}
