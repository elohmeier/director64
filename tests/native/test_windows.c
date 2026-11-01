// Synthetic MIAW score, shared-object, input and compositing contracts.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t stage;
static lv_runtime_t values;
static wc_t cached, uncached;
static unsigned prepared, started, entered, stopped, stage_clicks, window_clicks;
static lv_flow_t prepare(lv_runtime_t *r, lv_frame_t *f) {
  (void)f; prepared++;
  assert(r->context != &stage);
  r->globals[0] = lv_list(r, 1, (lv_t[]){lv_text(r, "shared", false)}, false);
  return LV_RETURN;
}
static lv_flow_t start(lv_runtime_t *r, lv_frame_t *f) {
  if(f->pc++<700)return LV_CONTINUE;
  started++;
  lv_set(r, f, "text", lv_make(LV_MEMBER, DG_ID(2,1,1)), lv_text(r, "controller", false));
  return LV_RETURN;
}
static lv_flow_t enter(lv_runtime_t *r, lv_frame_t *f) {
  (void)r; (void)f; entered++; return LV_RETURN;
}
static lv_flow_t stop(lv_runtime_t *r, lv_frame_t *f) {
  (void)r; (void)f; stopped++; return LV_RETURN;
}
static lv_flow_t click(lv_runtime_t *r, lv_frame_t *f) {
  (void)f;
  if (r->context == &stage) stage_clicks++; else window_clicks++;
  return LV_RETURN;
}
static lv_flow_t launch(lv_runtime_t *r, lv_frame_t *f) {
  f->locals[0] = lv_text(r, "stage continuation", false);
  lv_t win = lv_call(r, f, "window", 1, (lv_t[]){lv_text(r,"control",false)});
  lv_set(r, f, "filename", win, lv_text(r, "@:global:control.dir", false));
  assert(prepared == 1 && started == 1);
  assert(r->context == &stage && !strcmp(lv_cstr(r, f->locals[0]), "stage continuation"));
  assert(!strcmp(lv_cstr(r,lv_at(r,r->globals[0],1)),"shared"));
  return LV_RETURN;
}
static const lv_handler_t control_handlers[] = {
  {.name="preparemovie",.kind="MovieScript",.step=prepare},
  {.name="startmovie",.kind="MovieScript",.step=start},
  {.name="enterframe",.kind="MovieScript",.step=enter},
  {.name="stopmovie",.kind="MovieScript",.step=stop},
  {.name="mouseup",.kind="MovieScript",.step=click},
};
static const lv_handler_t stage_handlers[] = {
  {.name="startmovie",.kind="MovieScript",.locals=1,.step=launch},
  {.name="mouseup",.kind="MovieScript",.step=click},
};
static const lv_movie_t stage_code={.name="STAGE.DXR",.handlers=stage_handlers,.count=2};
static const lv_movie_t control_code={.name="CONTROL.DXR",.handlers=control_handlers,.count=5};
static const dg_cast_t stage_cast={.name="Internal",.file=1,.cast=1};
static const dg_cast_t control_cast={.name="Internal",.file=2,.cast=1};
static const dg_member_t field={.id=DG_ID(2,1,1),.number=1,.cast=1,.type=3,.text="authored"};
static const dg_frame_t frames[]={{0,0},{0,0},{0,0}};
static const dg_delta_t deltas[]={{0}};
static const dg_movie_t control={.id=2,.code=&control_code,.casts=&control_cast,.cast_count=1,
  .members=&field,.member_count=1,.frames=frames,.frame_count=3,.deltas=deltas,
  .stage_color=0x112233,.tempo=10};
static const dg_movie_t movie={.id=1,.code=&stage_code,.casts=&stage_cast,.cast_count=1,
  .frames=frames,.frame_count=3,.deltas=deltas,.stage_color=0x010203,.tempo=60};
static const dg_movie_t *find(void *context,const char *name,unsigned file) {
  (void)context;
  if (file==2 || (name && (!strcmp(name,"control") || !strcmp(name,"CONTROL")))) return &control;
  return file==1 ? &movie : NULL;
}
static bool absent(void *context,const char *name) {(void)context;(void)name;return false;}
static lv_t call(const char *name,unsigned n,const lv_t *a) {
  lv_t out={0};bool yield=false;
  assert(values.services.call(&values,name,n,a,&out,&yield));
  assert(!values.failed);return out;
}
static void tick(int x,int y,bool down) {
  if(!dg_tick(&stage,x,y,down,DG_SERVICE_BUDGET))fprintf(stderr,"%s\n",values.error);
  assert(!values.failed && !values.script_error);
}
static void render(void) {
  wc_render(&cached,&stage,0,0,false,0);
  wc_render(&uncached,&stage,0,0,false,0);
  assert(!values.failed && !memcmp(cached.pixels,uncached.pixels,sizeof(cached.pixels)));
}
int main(void) {
  dg_init(&stage,&values,(dg_platform_t){.find_movie=find,.file_exists=absent},NULL,NULL,0,1);
  assert(dg_enter(&stage,&movie,1,NULL,0));
  assert(dg_service(&stage,DG_SERVICE_BUDGET));
  assert(stage.movie==&movie && stage.window_runtime && stage.window_runtime->movie==&control);
  lv_set(&values,NULL,"idlehandlerperiod",(lv_t){0},lv_num(6));
  dg_runtime_t *idle_window=dg_window_begin(&stage);
  assert(idle_window && lv_integer(&values,lv_get(&values,NULL,"idlehandlerperiod",(lv_t){0}))==1);
  lv_set(&values,NULL,"idlehandlerperiod",(lv_t){0},lv_num(2));
  dg_window_end(&stage);
  assert(lv_integer(&values,lv_get(&values,NULL,"idlehandlerperiod",(lv_t){0}))==6);
  lv_t numeric=lv_list(&values,2,(lv_t[]){lv_num(1),lv_num(2)},true);
  assert(lv_integer(&values,call("fx_errornumber",1,&numeric))==0 && !values.failed);
  assert(lv_truth(&values,call("fx_fileexists",2,(lv_t[]){(lv_t){0},lv_text(&values,"C:\\global\\control.DXR",false)})));
  assert(!lv_truth(&values,call("fx_fileexists",2,(lv_t[]){(lv_t){0},lv_text(&values,"C:\\global\\missing.DXR",false)})));
  assert(!lv_truth(&values,call("fx_fileexists",2,(lv_t[]){(lv_t){0},lv_text(&values,"C:\\global\\control.WAV",false)})));
  assert(!stage.window_visible && prepared==1 && started==1 && entered==1);
  unsigned before=entered;
  for(unsigned i=0;i<60;i++)tick(-1,-1,false);
  assert(entered-before==10 && stage.window_runtime->ticks==stage.ticks);
  // Window-only cast members retain a single mutable identity when the stage
  // calls the controller's shared interfaces, including after heap collection.
  lv_t member=lv_make(LV_MEMBER,DG_ID(2,1,1));
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"text",member)),"controller"));
  lv_set(&values,NULL,"text",member,lv_text(&values,"statistics",false));
  lv_collect(&values);
  dg_runtime_t *child=dg_window_begin(&stage);
  assert(child && !strcmp(lv_cstr(&values,lv_get(&values,NULL,"text",member)),"statistics"));
  dg_window_end(&stage);
  lv_t range=lv_list(&values,8,(lv_t[]){lv_symbol(&values,"_textRange"),member,
      lv_symbol(&values,"kind"),lv_symbol(&values,"char"),lv_symbol(&values,"first"),lv_num(1),
      lv_symbol(&values,"last"),lv_num(2)},true);
  values.globals[1]=range;
  lv_set(&values,NULL,"fontsize",range,lv_num(0));
  assert(lv_integer(&values,lv_get(&values,NULL,"fontsize",range))==0);
  assert(values.services.text_splice(&values,member,"char",lv_num(2),lv_num(2),
      lv_text(&values,"ZZ",false),lv_text(&values,"sZZatistics",false)));
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"text",member)),"sZZatistics"));
  assert(lv_integer(&values,lv_get(&values,NULL,"fontsize",range))==0);
  child=dg_window_begin(&stage);
  values.globals[2]=call("image",3,(lv_t[]){lv_num(2),lv_num(2),lv_num(32)});
  call("setpixel",4,(lv_t[]){values.globals[2],lv_num(0),lv_num(0),lv_make(LV_COLOR,0x123456)});
  dg_window_end(&stage);
  assert(lv_integer(&values,call("getpixel",3,(lv_t[]){values.globals[2],lv_num(0),lv_num(0)}))==0x123456);
  // Nested tell stage restores its enclosing controller target.
  lv_t win=call("window",1,(lv_t[]){lv_text(&values,"control",false)});
  call("tell_window",1,&win);
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"moviename",(lv_t){0})),"CONTROL.DXR"));
  call("tell_stage",0,NULL);
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"moviename",(lv_t){0})),"STAGE.DXR"));
  call("tell_end",0,NULL);
  dg_key(&stage,59,0,true);dg_key(&stage,56,0,true);dg_key(&stage,58,0,true);
  assert(lv_truth(&values,lv_get(&values,NULL,"commanddown",(lv_t){0})));
  assert(lv_truth(&values,lv_get(&values,NULL,"controldown",(lv_t){0})));
  call("tell_window",1,&win);
  assert(lv_truth(&values,lv_get(&values,NULL,"shiftdown",(lv_t){0})));
  assert(lv_truth(&values,lv_get(&values,NULL,"optiondown",(lv_t){0})));
  lv_set(&values,NULL,"volume",lv_make(LV_SOUND,1),lv_num(137));
  assert(stage.channel_volume[0]==137);
  call("tell_end",0,NULL);
  dg_key(&stage,59,0,false);dg_key(&stage,56,0,false);dg_key(&stage,58,0,false);
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"moviename",(lv_t){0})),"CONTROL.DXR"));
  call("tell_end",0,NULL);
  lv_set(&values,NULL,"rect",win,lv_rect(&values,4,3,10,8));
  lv_set(&values,NULL,"visible",win,lv_num(1));
  wc_init(&cached,(wc_host_t){0},1024);wc_init(&uncached,(wc_host_t){0},1024);
  wc_cache_enabled(&uncached,false);render();
  assert(cached.pixels[3*WC_WIDTH+4]==0xff332211u);
  assert(cached.pixels[3*WC_WIDTH+10]==0xff030201u);
  assert(cached.pixels[8*WC_WIDTH+4]==0xff030201u);
  // A press and release belong to the panel, including release after dragging
  // outside. The underlying stage receives neither event.
  tick(5,4,true);tick(30,30,false);tick(30,30,false);
  assert(window_clicks==1 && stage_clicks==0);
  tick(30,30,true);tick(30,30,false);tick(30,30,false);
  assert(stage_clicks==1 && window_clicks==1);
  lv_set(&values,NULL,"visible",win,lv_num(0));render();
  assert(cached.pixels[3*WC_WIDTH+4]==0xff030201u);
  before=entered;for(unsigned i=0;i<60;i++)tick(-1,-1,false);assert(entered-before==10);
  assert(!dg_edit_text(&stage,DG_WINDOW_SPRITE|1,"42"));
  child=dg_window_begin(&stage);
  child->sprites[1]=(dg_sprite_t){.value={.type=1,.member=DG_ID(2,1,1),.width=20,.height=20,.blend=100},
      .visible=true,.puppet=true,.stretch=true};
  lv_t behavior=lv_instance(&values,lv_make(LV_SCRIPT,DG_ID(2,1,1)));
  values.globals[3]=behavior;
  lv_set(&values,NULL,"pCurrentLine",behavior,lv_num(2));
  values.roots[DG_BEHAVIOR_ROOT+1]=lv_list(&values,1,&behavior,false);
  lv_t sprite=lv_make(LV_SPRITE,1);
  assert(lv_integer(&values,lv_index_get(&values,sprite,lv_symbol(&values,"pCurrentLine")))==2);
  lv_index_set(&values,sprite,lv_text(&values,"pCurrentLine",false),lv_num(3));
  assert(lv_integer(&values,lv_get(&values,NULL,"pCurrentLine",behavior))==3);
  lv_set(&values,NULL,"editable",member,lv_num(1));
  dg_window_end(&stage);
  lv_set(&values,NULL,"visible",win,lv_num(1));
  assert(dg_edit_text(&stage,DG_WINDOW_SPRITE|1,"42"));
  for(unsigned i=0;i<20;i++)tick(-1,-1,false);
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"text",member)),"42"));
  assert(!stage.text_pending && !stage.window_runtime->text_pending);
  // Forget releases the independent heap roots/runtime at a service boundary.
  lv_get(&values,NULL,"forget",win);
  assert(!lv_truth(&values,call("windowpresent",1,(lv_t[]){lv_text(&values,"control",false)})));
  tick(-1,-1,false);
  assert(!stage.window_runtime && !stage.window_movie && stopped==1);
  assert(lv_integer(&values,call("getpixel",3,(lv_t[]){values.globals[2],lv_num(0),lv_num(0)}))==0x123456);
  dg_linked_dispose(&stage);dg_images_dispose(&stage);dg_films_dispose(&stage);
  wc_release(&cached);wc_release(&uncached);
  puts("MIAW independent score, shared globals/member roots, tells, input and rendering passed");
}
