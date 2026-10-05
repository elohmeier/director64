#include "controls.h"
#include <assert.h>
#include <stdio.h>
static dg_runtime_t d;
static lv_runtime_t r;
static const char *const globals[]={"gobjektreferens","lo_state"};
static const lv_movie_t kr_code={.name="KR.DXR"}, kb_code={.name="KB.DXR"}, lo_code={.name="LO.DXR"};
static const dg_movie_t kr={.code=&kr_code}, kb={.code=&kb_code}, lo={.code=&lo_code};
static bool held(unsigned c){return !!(d.keys_down[c>>5]&(UINT32_C(1)<<(c&31)));}
static void pad(input_sample_t *s){mucklas_pad_keys(&d,s);assert(!r.failed);}
int main(void){
  dg_init(&d,&r,(dg_platform_t){0},NULL,globals,2,42);d.movie=&kr;
  lv_t driver=lv_instance(&r,lv_make(LV_SCRIPT, 1));r.roots[0]=driver;
  lv_set(&r,NULL,"spelarnr",driver,lv_num(2));
  lv_t bindings=lv_list(&r,0,NULL,true);r.roots[1]=bindings;
  lv_set(&r,NULL,"upp",bindings,lv_num(13));lv_set(&r,NULL,"hoger",bindings,lv_num(2));
  lv_set(&r,NULL,"ner",bindings,lv_num(1));lv_set(&r,NULL,"vanster",bindings,lv_text(&r,"a",false));
  lv_set(&r,NULL,"minatangenter",driver,bindings);
  r.globals[0]=lv_list(&r,0,NULL,true);
  lv_set(&r,NULL,"kotte",r.globals[0],lv_list(&r,1,&driver,false));
  input_sample_t s={.connected=true,.buttons=INPUT_C_UP,.pads[0]={.connected=true,.buttons=INPUT_LEFT|INPUT_UP}};pad(&s);
  assert(held(126)&&held(0)&&held(13)&&!held(123));
  s.pads[0].connected=false;pad(&s);assert(held(126)&&!held(0)&&!held(13));
  s.buttons=0;pad(&s);assert(!held(126));
  d.movie=&kb;s.buttons=INPUT_C_LEFT;s.pads[0].connected=true;s.pads[0].buttons=INPUT_LEFT|INPUT_A;pad(&s);
  assert(held(123)&&held(49));s.buttons=0;pad(&s);assert(held(123));
  s.pads[0].buttons=0;s.pads[0].stick_x=60;pad(&s);assert(held(124)&&!held(123)&&!held(49));
  d.movie=&lo;pad(&s);assert(!held(124));
  r.globals[1]=lv_text(&r,"skrivernamn",true);assert(mucklas_name_target(&d,39));assert(!mucklas_name_target(&d,40));
  r.globals[1]=lv_text(&r,"valjer",true);assert(!mucklas_name_target(&d,39));
  assert(mucklas_key_code('A')==0&&mucklas_key_code('z')==6&&mucklas_key_code(8)==51);
  assert(!r.failed);puts("Mucklas controls: localized player two, chords, disconnect and name target PASS");
}
