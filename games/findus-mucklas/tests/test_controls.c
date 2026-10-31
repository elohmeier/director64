#include "controls.h"
#include <assert.h>
#include <stdio.h>
static dg_runtime_t d;
static lv_runtime_t r;
static const char *const globals[]={"lo_state"};
static const lv_movie_t kr_code={.name="KR.DXR"}, kb_code={.name="KB.DXR"}, lo_code={.name="LO.DXR"};
static const dg_movie_t kr={.code=&kr_code}, kb={.code=&kb_code}, lo={.code=&lo_code};
static bool held(unsigned c){return !!(d.keys_down[c>>5]&(UINT32_C(1)<<(c&31)));}
static void pad(input_sample_t *s){mucklas_pad_keys(&d,s);assert(!r.failed);}
int main(void){
  dg_init(&d,&r,(dg_platform_t){0},NULL,globals,1,42);d.movie=&kr;
  input_sample_t s={.connected=true,.buttons=INPUT_C_UP|INPUT_C_LEFT};pad(&s);
  assert(held(126)&&held(123)&&!held(124));
  s.buttons=INPUT_C_UP|INPUT_C_RIGHT;pad(&s);assert(held(126)&&held(124)&&!held(123));
  s.connected=false;pad(&s);assert(!held(126)&&!held(124));
  s.connected=true;s.buttons=0;s.stick_x=60;pad(&s);assert(!held(124));
  s.buttons=INPUT_C_LEFT;pad(&s);assert(held(123));
  d.movie=&kb;pad(&s);assert(!held(123)&&!held(49));
  d.movie=&lo;pad(&s);
  r.globals[0]=lv_text(&r,"skrivernamn",true);assert(mucklas_name_target(&d,39));assert(!mucklas_name_target(&d,40));
  r.globals[0]=lv_text(&r,"valjer",true);assert(!mucklas_name_target(&d,39));
  assert(mucklas_key_code('A')==0&&mucklas_key_code('z')==6&&mucklas_key_code(8)==51);
  assert(!r.failed);puts("Mucklas controls: single-player chords, disconnect and name target PASS");
}
