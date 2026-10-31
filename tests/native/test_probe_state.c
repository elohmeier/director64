// Observation must cover the actual stage, including the lower/right controls
// of 800x600 games and the input controller's inclusive maximum coordinates.
#include "director.h"
#include "compositor.h"
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t director;
static lv_runtime_t values;
static char output[4096];
static size_t output_length;
static void emit(const char *format, ...) {
  va_list args; va_start(args,format);
  int n=vsnprintf(output+output_length,sizeof(output)-output_length,format,args);
  va_end(args); assert(n>=0 && (size_t)n<sizeof(output)-output_length);
  output_length+=(size_t)n;
}
static void raw_state(void) { emit("{\"movie\":\"SYNTHETIC\",\"nested\":{\"x\":1}}\n"); }
#include "../../platforms/web/probe_state.inc"
static const lv_movie_t code={.name="SYNTHETIC"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t frame={0};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=8,.shape=1,.filled=1};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
  .members=&member,.member_count=1,.frames=&frame,.frame_count=1};
static void clear(void) { output_length=0;output[0]=0; }
int main(void) {
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,42);
  assert(dg_enter(&director,&movie,1,NULL,0));
  director.sprites[1]=(dg_sprite_t){.visible=true,.stretch=true,.moveable=true,.value={
    .member=member.id,.type=16,.x=WC_WIDTH-45,.y=WC_HEIGHT-45,
    .width=30,.height=30,.blend=100,.flags=48}};
  dg_sprite_changed(&director,1);
  point(1); assert(strstr(output,"null")==NULL);
  int x=0,y=0; assert(sscanf(output,"{\"point\":[%d,%d]",&x,&y)==2);
  assert(x>WC_WIDTH-45 && y>WC_HEIGHT-45 && dg_mouse_hit(&director,x,y)==1);
  director.sprites[1].value.x=WC_WIDTH-10;
  director.sprites[1].value.y=WC_HEIGHT-10;
  director.sprites[1].value.width=director.sprites[1].value.height=1;
  dg_sprite_changed(&director,1); clear(); point(1);
  assert(strstr(output,"null")==NULL);
  director.sprites[1].value.x=WC_WIDTH+1;
  dg_sprite_changed(&director,1); clear(); point(1); assert(strstr(output,"null"));
  clear(); state(); char expected[64];
  snprintf(expected,sizeof(expected),"\"stage\":[%u,%u]",WC_WIDTH,WC_HEIGHT);
  assert(strstr(output,expected)); assert(strstr(output,"\"nested\":{\"x\":1}"));
  assert(strstr(output,"\"capabilities\":{\"keyboard\":"));
  puts("probe: full authored stage, inclusive input bounds and observation metadata passed");
}
