#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t d;
static lv_runtime_t r;
static const dg_film_layer_t layers[] = {
    {.name="sun", .asset="sun-a", .origin_x=688128, .origin_y=1327104,
     .value={.x=8,.y=18,.width=8,.height=10,.blend=100}},
    {.name="sun", .asset="sun-b", .origin_x=819200, .origin_y=1589248,
     .value={.x=10,.y=22,.width=12,.height=14,.blend=100}},
};
static const dg_film_frame_t film[] = {{1,layers}, {1,layers+1}, {0,NULL}};
static const dg_flash_helper_t helpers[] = {
    {.name="advance",.target="sun",.property="_y",.operation=1,.argument=0},
    {.name="relocate",.target="sun",.property="_x",.operation=2,.argument=1},
};
static const dg_text_style_t text_style = {.size=12};
static const dg_flash_field_t fields[] = {
    {.name="caption",.text="",.style=&text_style,.x=999,.y=998,.width=42,.height=24,
     .local_bounds={131072,196608,786432,917504}},
};
static const dg_flash_helper_t text_helpers[] = {
    {.name="captionResize",.target="caption",.property="size",.operation=3,.argument=0},
    {.name="captionUpdate",.target="caption",.property="text",.operation=4,.argument=0},
    {.name="applyContent",.target="caption",.property="text",.operation=4,.argument=0,.method=true},
    {.name="applySize",.target="caption",.property="size",.operation=3,.argument=0,.method=true},
};
static const dg_film_layer_t text_layers[] = {
    {.name="caption",.origin_x=688128,.origin_y=1327104,.affine=true,
     .matrix={0,131072,-196608,0,688128,1327104}},
    {.name="caption",.origin_x=819200,.origin_y=1589248,.affine=true,
     .matrix={65536,0,0,32768,819200,1589248}},
};
static const dg_film_frame_t text_film[] = {{1,text_layers},{1,text_layers+1},{0,NULL}};
static const dg_member_t members[] = {
    {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,.source_xtra=1,.width=100,.height=100,
     .film_count=3,.film_frames=film,.flash_helper_count=2,.flash_helpers=helpers},
    {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=2,.source_xtra=1,.width=100,.height=100,
     .film_count=3,.film_frames=film},
    {.id=DG_ID(1,1,3),.number=3,.cast=1,.type=2,.source_xtra=1,.width=100,.height=100,
     .film_count=3,.film_frames=text_film,.flash_field_count=1,.flash_fields=fields,
     .flash_helper_count=4,.flash_helpers=text_helpers},
};
static const lv_movie_t code = {.name="synthetic-flash"};
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static const dg_frame_t frames[] = {{0,0}};
static const dg_movie_t movie = {.id=1,.code=&code,.frame_count=1,.frames=frames,
    .cast_count=1,.casts=&cast,.member_count=3,.members=members};
static lv_t call(const char *name, unsigned n, const lv_t *args) {
  lv_frame_t frame = {.movie=&code};
  return lv_call(&r, &frame, name, n, args);
}
static double number(lv_t object, const char *property) {
  return lv_numeric(lv_get(&r, NULL, property, object));
}
int main(void) {
  dg_init(&d, &r, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&d, &movie, 1, NULL, 0));
  lv_t sprite = lv_make(LV_SPRITE, 1);
  lv_set(&r, NULL, "member", sprite, lv_make(LV_MEMBER, DG_ID(1,1,1)));
  d.sprites[1].value.x = 200;
  d.sprites[1].value.y = 300;
  lv_t sun = lv_get(&r, NULL, "sun", sprite);
  assert(lv_type(sun) == LV_PROPLIST);
  assert(number(sun,"_x") == 10.5 && number(sun,"_y") == 20.25);
  assert(number(sun,"_width") == 8 && number(sun,"_height") == 10);
  // Reading geometry must not freeze its authored animation.
  d.sprites[1].film_frame = 1;
  assert(number(sun,"_x") == 12.5 && number(sun,"_y") == 24.25);
  assert(number(sun,"_width") == 12 && number(sun,"_height") == 14);
  // A same-value write is still an explicit override of the timeline.
  lv_set(&r, NULL, "_x", sun, lv_num(12.5));
  d.sprites[1].film_frame = 0;
  assert(number(sun,"_x") == 12.5 && number(sun,"_y") == 20.25);
  call("setflashproperty", 4, (lv_t[]){sprite,lv_text(&r,"sun",false),
      lv_text(&r,"posY",true),lv_num(31.75)});
  assert(number(sun,"_y") == 31.75);
  assert(lv_numeric(call("getflashproperty",3,(lv_t[]){sprite,
      lv_text(&r,"sun",false),lv_text(&r,"posY",true)})) == 31.75);
  call("advance", 2, (lv_t[]){sprite,lv_num(-2.5)});
  assert(number(sun,"_y") == 29.25);
  call("relocate", 3, (lv_t[]){sprite,lv_num(999),lv_num(7.5)});
  assert(number(sun,"_x") == 7.5);
  lv_index_set(&r, sun, lv_text(&r,"_xscale",true), lv_num(200));
  assert(number(sun,"_width") == 16 && number(sun,"_xscale") == 200);
  d.sprites[1].film_frame = 1;
  assert(number(sun,"_width") == 24);
  lv_set(&r, NULL, "_width", sun, lv_num(6));
  assert(number(sun,"_xscale") == 50);
  d.sprites[1].film_frame = 0;
  assert(number(sun,"_width") == 6 && number(sun,"_xscale") == 75);
  d.sprites[1].film_frame = 2;
  assert(number(sun,"_visible") == 0);
  // Reusing a channel for another Flash member discards the old overrides.
  lv_set(&r, NULL, "member", sprite, lv_make(LV_MEMBER, DG_ID(1,1,2)));
  lv_t replacement = lv_get(&r, NULL, "sun", sprite);
  assert(lv_id(replacement) != lv_id(sun) && number(replacement,"_x") == 10.5);
  assert(number(replacement,"_y") == 20.25 && number(replacement,"_width") == 8);
  assert(d.flash_object_count == 1 && !r.failed);
  lv_set(&r,NULL,"member",sprite,lv_make(LV_MEMBER,DG_ID(1,1,3)));
  lv_t caption=lv_get(&r,NULL,"caption",sprite);
  assert(number(caption,"_x")==10.5 && number(caption,"_y")==20.25);
  assert(number(caption,"_width")==12 && number(caption,"_height")==14);
  assert(number(caption,"_xscale")==200 && number(caption,"_yscale")==300);
  assert(number(caption,"_rotation")==90);
  d.sprites[1].film_frame=1;
  assert(number(caption,"_x")==12.5 && number(caption,"_y")==24.25);
  assert(number(caption,"_xscale")==100 && number(caption,"_yscale")==50);
  assert(number(caption,"_rotation")==0);
  call("captionUpdate",2,(lv_t[]){sprite,lv_text(&r,"Gr%c3%bc%c3%9fe%21",false)});
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",caption)),"Grüße!"));
  call("captionResize",2,(lv_t[]){sprite,lv_num(18)});
  assert(number(caption,"size")==18 && number(caption,"textheight")>0);
  call("applyContent",2,(lv_t[]){caption,lv_text(&r,"hello%20world",false)});
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",caption)),"hello world"));
  call("applySize",2,(lv_t[]){caption,lv_num(24)});
  assert(number(caption,"size")==24);
  lv_set(&r,NULL,"_width",caption,lv_num(20));
  lv_set(&r,NULL,"_xscale",caption,lv_num(75));
  lv_set(&r,NULL,"_x",caption,lv_num(50));
  d.sprites[1].film_frame=0;
  assert(number(caption,"_width")==20 && number(caption,"_xscale")==75);
  assert(number(caption,"_x")==50 && number(caption,"_y")==20.25);
  d.sprites[1].film_frame=2;
  assert(number(caption,"_visible")==0);
  // An unconverted object must not manufacture parent geometry and succeed.
  lv_t missing = lv_get(&r, NULL, "missingChild", sprite);
  assert(lv_type(missing) == LV_VOID && r.failed);
  puts("Flash child geometry follows its timeline and explicit Lingo transforms");
}
