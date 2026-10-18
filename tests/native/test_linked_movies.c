#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t host;
static lv_runtime_t values;
static unsigned started, clicked, stopped, sound_plays, sound_stops, parent_calls, key_downs, key_ups;
static unsigned forbidden_events;
static lv_flow_t forbidden(lv_runtime_t *r, lv_frame_t *f) {
  (void)r; (void)f; forbidden_events++; return LV_RETURN;
}
static dg_runtime_t *key_receiver;
static lv_flow_t child_start(lv_runtime_t *r, lv_frame_t *f) {
  (void)f;
  // Shared global objects must retain identity across interpreter contexts.
  lv_t list = r->globals[0];
  lv_set_at(r, list, 1, lv_num(lv_id(lv_at(r, list, 1)) + 1));
  r->roots[DG_FIELD_TEXT_ROOT] = lv_text(r, "child root", false);
  lv_set(r, f, "keyboardfocussprite", (lv_t){0}, lv_num(1));
  lv_t image = lv_call(r, f, "image", 3, (lv_t[]){lv_num(1),lv_num(1),lv_num(32)});
  r->globals[started + 1] = image;
  lv_call(r, f, "setpixel", 4, (lv_t[]){image,lv_num(0),lv_num(0),lv_make(LV_COLOR,0x112233)});
  started++;
  return LV_RETURN;
}
static lv_flow_t child_up(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->pc++) {
    f->locals[0] = lv_text(r, "suspended child", false);
    return LV_YIELD;
  }
  assert(!strcmp(lv_cstr(r, f->locals[0]), "suspended child"));
  assert(!strcmp(lv_cstr(r, r->roots[DG_FIELD_TEXT_ROOT]), "child root"));
  clicked++; return LV_RETURN;
}
static lv_flow_t child_stop(lv_runtime_t *r, lv_frame_t *f) {
  (void)r; (void)f; stopped++; return LV_RETURN;
}
static lv_flow_t child_keydown(lv_runtime_t *r,lv_frame_t *f) {
  (void)f;key_downs++;key_receiver=r->context;return LV_RETURN;
}
static lv_flow_t child_keyup(lv_runtime_t *r,lv_frame_t *f) {
  (void)f;key_ups++;key_receiver=r->context;return LV_RETURN;
}
static const lv_handler_t handlers[] = {
  {.name="beginsprite",.member=2,.cast="Internal",.kind="ScoreScript",.arguments=1,.locals=1,.step=child_start},
  {.name="mouseup",.member=2,.cast="Internal",.kind="ScoreScript",.arguments=1,.locals=1,.step=child_up},
  {.name="endsprite",.member=2,.cast="Internal",.kind="ScoreScript",.arguments=1,.locals=1,.step=child_stop},
  {.name="keydown",.member=2,.cast="Internal",.kind="ScoreScript",.arguments=1,.locals=1,.step=child_keydown},
  {.name="keyup",.member=2,.cast="Internal",.kind="ScoreScript",.arguments=1,.locals=1,.step=child_keyup},
  {.name="preparemovie",.member=4,.cast="Internal",.kind="MovieScript",.step=forbidden},
  {.name="startmovie",.member=4,.cast="Internal",.kind="MovieScript",.step=forbidden},
  {.name="stopmovie",.member=4,.cast="Internal",.kind="MovieScript",.step=forbidden},
  {.name="enterframe",.member=4,.cast="Internal",.kind="ScoreScript",.step=forbidden},
  {.name="exitframe",.member=4,.cast="Internal",.kind="ScoreScript",.step=forbidden},
};
static const lv_movie_t child_code = {.name="CHILD.DXR",.count=10,.handlers=handlers};
static lv_flow_t parent_method(lv_runtime_t *r,lv_frame_t *f) {
  (void)f; assert(r->context!=&host); parent_calls++; return LV_RETURN;
}
static const lv_handler_t parent_handler={.name="ping",.member=4,.cast="Internal",
  .kind="ParentScript",.arguments=1,.locals=1,.step=parent_method};
static const lv_movie_t host_code = {.name="HOST.DXR",.count=1,.handlers=&parent_handler};
static const dg_cast_t host_cast = {.name="Internal",.file=1,.cast=1}, child_cast = {.name="Internal",.file=2,.cast=1};
static const dg_member_t host_members[] = {
  {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=9,.width=4,.height=4,.linked_movie=2,
   .linked_flags=0x1a,.linked_left=10,.linked_top=20},
  {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=9,.width=4,.height=4,.linked_movie=2,
   .linked_flags=0x22,.linked_left=10,.linked_top=20},
  {.id=DG_ID(1,1,3),.number=3,.cast=1,.type=6,.samples=100},
  {.id=DG_ID(1,1,4),.number=4,.cast=1,.type=11},
  {.id=DG_ID(1,1,5),.number=5,.cast=1,.type=8,.width=8,.height=8,.shape=1,.filled=1},
};
static const dg_member_t child_members[] = {
  {.id=DG_ID(2,1,1),.number=1,.cast=1,.type=3,.width=2,.height=2,.text="",.editable=true},
  {.id=DG_ID(2,1,2),.number=2,.cast=1,.type=11},
  {.id=DG_ID(2,1,3),.number=3,.cast=1,.type=6,.samples=100},
  {.id=DG_ID(2,1,4),.number=4,.cast=1,.type=11},
};
static const dg_behavior_t child_behavior={.script=DG_ID(2,1,2)};
static const dg_delta_t child_deltas[] = {
  {.channel=6,.mask=DG_ALL,.value={.type=1,.member=DG_ID(2,1,1),.width=2,.height=2,
    .x=10,.y=20,.ink=0,.blend=100,.flags=16,.fore_rgb=0xff0000,
    .behavior_count=1,.behaviors=&child_behavior}},
  {.channel=0,.mask=DG_ALL,.value={.script=DG_ID(2,1,4)}},
  {.channel=1,.mask=DG_ALL,.value={.tempo=3,.delay=1}},
  {.channel=6,.mask=DG_POSITION,.value={.type=1,.member=DG_ID(2,1,1),.width=2,.height=2,
    .x=12,.y=22,.ink=0,.blend=100,.flags=16,.fore_rgb=0xff0000,
    .behavior_count=1,.behaviors=&child_behavior}},
};
static const dg_delta_t host_delta[] = {
  {.channel=6,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,1),.width=8,.height=8,
    .x=40,.y=40,.ink=0,.blend=100,.stretch=1}},
  {.channel=7,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,1),.width=8,.height=8,
    .x=80,.y=40,.ink=0,.blend=100,.stretch=1}},
  {.channel=8,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,2),.width=8,.height=8,
    .x=120,.y=40,.ink=0,.blend=100,.stretch=1}},
};
static const dg_frame_t host_frames[]={{0,3},{3,0}}, child_frames[]={{0,3},{3,1}};
static const uint32_t host_palette[256]={[1]=0xff0000}, child_palette[256]={[1]=0x0000ff};
static const dg_movie_t child_movie={.id=2,.code=&child_code,.casts=&child_cast,.cast_count=1,
  .members=child_members,.member_count=4,.tempo=7,.frame_count=2,.frames=child_frames,.deltas=child_deltas,
  .palette=child_palette};
static const dg_movie_t host_movie={.id=1,.code=&host_code,.casts=&host_cast,.cast_count=1,
  .members=host_members,.member_count=5,.tempo=60,.frame_count=2,.frames=host_frames,.deltas=host_delta,
  .palette=host_palette};
static const dg_movie_t *find(void *ctx,const char *stem,unsigned file) {
  (void)ctx; (void)stem; return file==2?&child_movie:file==1?&host_movie:NULL;
}
static void sound(void *ctx,unsigned channel,const dg_member_t *m) {
  (void)ctx; assert(channel==0);
  if(m) sound_plays++; else sound_stops++;
}
static uint32_t pixel(lv_t image) {
  lv_frame_t f={.movie=values.current};
  return (uint32_t)lv_id(lv_call(&values,&f,"getpixel",3,(lv_t[]){image,lv_num(0),lv_num(0)}));
}
static void tick(int x,int y,bool down) {
  if (!dg_tick(&host,x,y,down,DG_SERVICE_BUDGET)) fprintf(stderr,"linked failure %s\n",values.error);
  assert(!values.failed);
}
static void call(const char *name,unsigned argc,const lv_t *args) {
  lv_t out=0;bool yield=false;
  assert(values.services.call(&values,name,argc,args,&out,&yield));
  assert(!values.failed && values.context==&host);
}
int main(void) {
  static const char *const names[]={"shared","firstImage","secondImage"};
  dg_init(&host,&values,(dg_platform_t){.find_movie=find,.sound=sound},NULL,names,3,1);
  values.globals[0]=lv_list(&values,1,(lv_t[]){lv_num(0)},false);
  assert(dg_enter(&host,&host_movie,1,NULL,0));
  values.roots[DG_FIELD_TEXT_ROOT]=lv_text(&values,"host root",false);
  tick(0,0,false);
  assert(started==2 && lv_id(lv_at(&values,values.globals[0],1))==2);
  assert(!strcmp(lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT]),"host root"));
  assert(values.current==&host_code && values.context==&host);
  const dg_runtime_t *first=dg_linked_movie(&host,1),*second=dg_linked_movie(&host,2),*passive=dg_linked_movie(&host,3);
  assert(first && second && passive && first!=second && first->values==&values);
  assert(!forbidden_events && first->tempo==host.tempo && !first->score_delay);
  assert(dg_stage_palette(first)==host_palette && dg_stage_palette(second)==host_palette);
  assert(values.globals[1]!=values.globals[2]);
  assert(pixel(values.globals[1])==0x112233 && pixel(values.globals[2])==0x112233);
  lv_t parent_object=lv_instance(&values,lv_make(LV_SCRIPT,DG_ID(1,1,4)));
  dg_runtime_t *audio_child=dg_linked_begin(&host,1);
  assert(lv_start_method(&values,parent_object,"ping",0,NULL));
  lv_run(&values,DG_SERVICE_BUDGET);
  assert(parent_calls==1 && !values.failed);
  assert(dg_member(audio_child,DG_ID(1,1,3))==&host_members[2]);
  lv_t out=0;bool yielded=false;
  assert(values.services.call(&values,"puppetsound",2,
      (lv_t[]){lv_num(1),lv_make(LV_MEMBER,DG_ID(2,1,3))},&out,&yielded));
  assert(audio_child && sound_plays==1);
  dg_linked_end(&host);
  call("puppetsound",2,(lv_t[]){lv_num(1),lv_make(LV_MEMBER,DG_ID(1,1,3))});
  assert(sound_plays==2);
  call("gotoandstop",2,(lv_t[]){lv_make(LV_SPRITE,1),lv_num(2)});
  assert(first->frame==2 && first->linked_paused);
  unsigned second_frame=second->frame;
  tick(0,0,false);
  assert(first->frame==2 && second->frame!=second_frame);
  // A stopped playhead still dispatches input to its retained sprites.
  unsigned before_click=clicked;
  tick(45,45,true);tick(45,45,false);tick(45,45,false);
  assert(clicked==before_click+1 && first->frame==2 && first->linked_paused);
  for(unsigned i=0;i<4;i++)tick(0,0,false);
  assert(passive->frame==2 && passive->linked_paused);
  // Child input coordinates are mapped through the containing sprite box.
  call("play",1,(lv_t[]){lv_make(LV_SPRITE,1)});
  tick(41,41,true);tick(41,41,false);
  assert(first->mouse_x>=10 && first->mouse_x<14 && first->mouse_y>=20 && first->mouse_y<24);
  lv_collect(&values); // suspended child locals and host roots both survive
  tick(41,41,false);
  assert(clicked>=1);
  // Preserve the press until physical release even when dragging outside.
  tick(41,41,true);tick(0,0,true);
  assert(first->mouse_down);
  tick(0,0,false);assert(!first->mouse_down);
  // Occluding stage sprites block input to the child below them.
  host.sprites[4]=host.sprites[1];
  host.sprites[4].value.member=DG_ID(1,1,5);
  host.sprites[4].value.loc_z=99;
  host.sprites[4].puppet=true;
  dg_sprite_changed(&host,4);
  tick(41,41,true);assert(!first->mouse_down);
  tick(41,41,false);
  host.sprites[4].visible=false;dg_sprite_changed(&host,4);
  // Rotation and flips use the same source mapping as composition.
  host.sprites[1].puppet=true;
  lv_set(&values,NULL,"rotation",lv_make(LV_SPRITE,1),lv_num(90));
  tick(39,41,true);
  assert(first->mouse_down && first->mouse_x==10 && first->mouse_y==20);
  tick(39,41,false);
  lv_set(&values,NULL,"rotation",lv_make(LV_SPRITE,1),lv_num(90));
  lv_set(&values,NULL,"fliph",lv_make(LV_SPRITE,1),lv_num(1));
  tick(39,41,true);
  assert(first->mouse_down && first->mouse_x==13 && first->mouse_y==20);
  tick(39,41,false);
  // Keys route to the selected child; release stays with its original target
  // if another child is selected while the key remains held.
  dg_key(&host,0,'a',true);tick(0,0,false);
  assert(key_downs==1 && key_receiver==first && !host.key_count);
  tick(81,41,true);tick(81,41,false);
  dg_key(&host,0,'a',false);tick(0,0,false);
  assert(key_ups==1 && key_receiver==first);
  dg_key(&host,1,'s',true);tick(0,0,false);
  assert(key_downs==2 && key_receiver==second);
  dg_key(&host,1,'s',false);tick(0,0,false);
  assert(key_ups==2 && key_receiver==second);
  assert(!strcmp(lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT]),"host root"));
  // A child follows host frame opportunities even when its own score asks
  // for a different tempo or delay. Servicing input does not advance it.
  host.puppet_tempo=1;
  host.frame_deadline=host.clock+60000000;
  unsigned held_frame=first->frame;
  for(unsigned i=0;i<8;i++)tick(0,0,false);
  assert(first->frame==held_frame);
  host.puppet_tempo=60;host.frame_deadline=host.clock;
  tick(0,0,false);
  assert(first->frame!=held_frame && first->tempo==60 && !first->score_delay);
  lv_set(&values,NULL,"visible",lv_make(LV_SPRITE,1),lv_num(0));
  tick(0,0,false);
  assert(!dg_linked_movie(&host,1) && stopped>=1);
  assert(sound_stops==0); // child cleanup must preserve the newer host sound
  values.steps++;lv_collect(&values);
  assert(pixel(values.globals[1])==0x112233); // survives creating-child removal
  dg_linked_dispose(&host);dg_images_dispose(&host);
  assert(!values.external_roots);
  assert(!forbidden_events);
  puts("linked movies: shared globals, separate score/roots/stacks, controls, passive flags, input and lifecycle passed");
}
