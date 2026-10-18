// System cursor IDs/roles from Macromedia's scripting reference, corroborated
// by the selected 8.5.1 engine resource directory. No original cursor pixels.
#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t director;
static lv_runtime_t values;
static const lv_movie_t code = {.name="CURSOR-ROLES.DXR"};
static const dg_frame_t frame = {0};
static const dg_movie_t movie = {.id=1,.code=&code,.frame_count=1,.frames=&frame};
static const int roles[] = {
  254,256,257,258,259,260,261,262,263,264,265,266,267,268,269,
  271,272,280,281,284,285,286,290,291,292,293,294,295,296,297,
  298,299,300,301,302,303,304
};
static void set_cursor(int resource) {
  lv_t argument=lv_num(resource),result=0;
  bool yield=false;
  assert(values.services.call(&values,"cursor",1,&argument,&result,&yield));
  assert(!yield);
}
int main(void) {
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&director,&movie,1,NULL,0));
  dg_cursor_bitmap_t arrow;
  assert(dg_cursor_builtin(&arrow,-1));
  uint32_t hashes[sizeof(roles)/sizeof(*roles)];
  for(unsigned i=0;i<sizeof(roles)/sizeof(*roles);i++) {
    int resource=roles[i];
    assert(dg_cursor_supported(resource));
    set_cursor(resource);
    assert(!values.failed&&director.cursor.resource==resource&&!director.cursor.image);
    assert(lv_integer(&values,lv_get(&values,NULL,"cursor",0))==resource);
    lv_t sprite=lv_make(LV_SPRITE,1);
    lv_set(&values,NULL,"cursor",sprite,lv_num(resource));
    assert(!values.failed&&director.sprites[1].cursor.resource==resource);
    assert(lv_integer(&values,lv_get(&values,NULL,"cursor",sprite))==resource);
    dg_cursor_bitmap_t bitmap;
    uint16_t pixels[256];
    assert(dg_cursor_builtin(&bitmap,resource));
    assert(bitmap.hot_x<16&&bitmap.hot_y<16&&!bitmap.color);
    dg_cursor_pixels(&bitmap,pixels);
    unsigned black=0,white=0,clear=0;
    for(unsigned n=0;n<256;n++) {
      black+=pixels[n]==1;white+=pixels[n]==0xffff;clear+=pixels[n]==0;
    }
    assert(black&&white&&clear&&black+white+clear==256);
    hashes[i]=dg_cursor_bitmap_hash(&bitmap);
    assert(hashes[i]!=dg_cursor_bitmap_hash(&arrow));
    for(unsigned n=0;n<i;n++)assert(hashes[n]!=hashes[i]);
    if(resource==280)assert(bitmap.hot_x==4&&bitmap.hot_y==0&&pixels[4]);
    if(resource==260||resource==290||resource==291)
      assert(bitmap.hot_x==4&&bitmap.hot_y==6);
  }
  // Provider and accepted-ID validation agree, including the holes in the
  // standard ID range. An unrecognized resource leaves the old cursor intact.
  for(int id=-3;id<=320;id++) {
    dg_cursor_bitmap_t bitmap;
    assert(dg_cursor_supported(id)==dg_cursor_builtin(&bitmap,id));
  }
  dg_cursor_t before=director.cursor;
  set_cursor(270);
  assert(values.failed&&dg_cursor_equal(before,director.cursor));
  assert(strstr(values.error,"unsupported cursor resource")&&strstr(values.error,"270"));
  printf("cursor roles D%d: numeric identity, selection, distinct glyphs, hotspots and validation passed\n",
      DIRECTOR64_DIRECTOR_VERSION);
  return 0;
}
