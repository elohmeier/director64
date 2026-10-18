// Synthetic film scores exercise the independently advancing sound channels.
#include "director.h"
#include <assert.h>
#include <stdio.h>
static dg_runtime_t d;
static lv_runtime_t r;
static uint32_t playing[2];
static unsigned plays[2];
static void sound(void *ctx,unsigned ch,const dg_member_t *member) {
  (void)ctx;assert(ch<2);playing[ch]=member?member->id:0;plays[ch]++;
}
static const lv_movie_t code={.name="FILM-SOUND.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
#define ID(n) DG_ID(1,1,n)
static const uint32_t lower[]={ID(3),ID(4), ID(3),ID(4), 0,0};
static const uint32_t higher[]={ID(4),0, ID(4),0, 0,0};
static const uint8_t child_one[]={0,0,65,0,1,0,33,0,16,0};
static const uint8_t child_two[]={0,0,65,0,2,0,33,0,16,0};
static const dg_film_track_t nested_tracks[]={
  {1,1,sizeof(child_one),child_one},{2,1,sizeof(child_two),child_two},
};
static const dg_member_t members[]={
  {.id=ID(1),.number=1,.cast=1,.type=2,.film_count=3,.film_loop=1,.film_sounds=lower,.film_flags=8},
  {.id=ID(2),.number=2,.cast=1,.type=2,.film_count=3,.film_loop=1,.film_sounds=higher,.film_flags=8},
  {.id=ID(3),.number=3,.cast=1,.type=6,.samples=600},
  {.id=ID(4),.number=4,.cast=1,.type=6,.samples=600},
  {.id=ID(5),.number=5,.cast=1,.type=2,.film_count=3,.film_loop=1},
  {.id=ID(6),.number=6,.cast=1,.type=2,.film_count=3,.film_loop=1,.film_flags=8,
   .film_track_count=1,.film_tracks=nested_tracks},
  {.id=ID(7),.number=7,.cast=1,.type=2,.film_count=3,.film_loop=1,.film_flags=0,
   .film_track_count=1,.film_tracks=nested_tracks},
  {.id=ID(8),.number=8,.cast=1,.type=2,.film_count=3,.film_loop=1,.film_flags=8,
   .film_track_count=2,.film_tracks=nested_tracks},
  {.id=ID(9),.number=9,.cast=1,.type=2,.film_count=3,.film_loop=1,.film_flags=8,.film_sounds=lower,
   .film_track_count=2,.film_tracks=nested_tracks},
  {.id=ID(10),.number=10,.cast=1,.type=2,.film_count=3,.film_loop=1,.film_flags=8,.film_sounds=lower},
};
static const dg_delta_t deltas[]={
  {.channel=6,.mask=DG_ALL,.value={.type=1,.member=ID(1),.blend=100}},
  {.channel=7,.mask=DG_ALL,.value={.type=1,.member=ID(2),.blend=100}},
};
static const dg_frame_t frames[]={{0,2},{2,0},{2,0}};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
  .members=members,.member_count=10,.frames=frames,.frame_count=3,.deltas=deltas,.tempo=60};
static void call(const char *name,unsigned argc,const lv_t *a) {
  lv_t out=0;bool yield=false;assert(r.services.call(&r,name,argc,a,&out,&yield));
  dg_update_stage(&d);assert(!r.failed);
}
static void pose(unsigned sprite,unsigned frame) {
  call("gotoframe",2,(lv_t[]){lv_make(LV_SPRITE,sprite),lv_num(frame)});
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){.sound=sound},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  assert(playing[0]==ID(4) && playing[1]==ID(4)); // highest film supplies ch1
  unsigned previous=plays[0];dg_update_stage(&d);assert(plays[0]==previous);
  // Replacing a root with the same member before the next sound update still
  // starts a new instance. Its generation is independent of encoded file IDs.
  lv_set(&r,NULL,"member",lv_make(LV_SPRITE,2),lv_num(0));
  lv_set(&r,NULL,"member",lv_make(LV_SPRITE,2),lv_make(LV_MEMBER,ID(2)));
  dg_update_stage(&d);assert(playing[0]==ID(4) && plays[0]==previous+1);
  previous=plays[0];
  pose(2,2);assert(plays[0]==previous); // sustained source does not retrigger
  pose(2,1);assert(plays[0]==previous+1); // rewind starts its sound again
  d.score[4].member=ID(3);dg_update_stage(&d);
  assert(playing[0]==ID(3)); // authored main score takes priority
  assert(lv_id(lv_get(&r,NULL,"framesound1",0))==3);
  assert(lv_id(lv_get(&r,NULL,"framesound2",0))==0);
  d.score[4].member=0;dg_update_stage(&d);assert(playing[0]==ID(4));
  pose(2,3);assert(playing[0]==ID(3) && playing[1]==ID(4));
  pose(1,3);assert(!playing[0] && !playing[1]);
  call("puppetsound",2,(lv_t[]){lv_num(1),lv_make(LV_MEMBER,ID(3))});
  pose(2,1);assert(playing[0]==ID(3)); // film cannot replace puppeted audio
  call("stop",1,(lv_t[]){lv_make(LV_SOUND,1)});assert(playing[0]==ID(4));
  call("gotoandstop",2,(lv_t[]){lv_make(LV_SPRITE,2),lv_num(2)});
  previous=plays[0];
  for(unsigned i=0;i<5;i++) assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));
  assert(d.sprites[2].film_frame==1 && plays[0]==previous);
  // Disabled sound is represented by no event table in the converted member.
  lv_set(&r,NULL,"member",lv_make(LV_SPRITE,2),lv_make(LV_MEMBER,ID(5)));
  pose(1,3);dg_update_stage(&d);assert(!playing[0] && !playing[1]);
  lv_set(&r,NULL,"member",lv_make(LV_SPRITE,1),lv_num(0));
  lv_set(&r,NULL,"member",lv_make(LV_SPRITE,2),lv_num(0));
  lv_t parent=lv_make(LV_SPRITE,9);
  lv_set(&r,NULL,"member",parent,lv_make(LV_MEMBER,ID(6)));
  d.sprites[9].value.type=16;
  call("updatestage",0,NULL);
  lv_t child=dg_film_tell(&d,parent,1);
  assert(dg_film_frame(&d,child,false)==2 && playing[0]==ID(3) && playing[1]==ID(4));
  // Local animation edits preserve the nested sound source and playhead,
  // even while the enclosing film is paused.
  call("stop",1,&parent);
  unsigned sustained[2]={plays[0],plays[1]};
  for(unsigned i=0;i<3;i++) {
    lv_set(&r,NULL,"loc",child,lv_point(&r,20+i,30+i));
    lv_set(&r,NULL,"blend",child,lv_num(80-i));
    lv_set(&r,NULL,"ink",child,lv_num(8));
    lv_set(&r,NULL,"width",child,lv_num(10+i));
    dg_update_stage(&d);
    assert(!r.failed&&dg_film_frame(&d,child,false)==2);
    assert(plays[0]==sustained[0]&&plays[1]==sustained[1]);
  }
  previous=plays[0];
  lv_set(&r,NULL,"member",child,lv_make(LV_MEMBER,ID(10)));
  dg_update_stage(&d);
  assert(playing[0]==ID(3) && plays[0]==previous+1); // recycled node slot is a fresh sound source

  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,child,false)==3 && !playing[0] && !playing[1]);
  pose(9,1);
  assert(dg_film_frame(&d,child,false)==1 && playing[0]==ID(3));
  d.score[4].member=ID(4);dg_update_stage(&d);assert(playing[0]==ID(4));
  d.score[4].member=0;
  lv_set(&r,NULL,"member",parent,lv_make(LV_MEMBER,ID(7)));
  call("updatestage",0,NULL);assert(!playing[0]&&!playing[1]); // disabled parent mutes descendants
  lv_set(&r,NULL,"member",parent,lv_make(LV_MEMBER,ID(8)));
  call("updatestage",0,NULL);assert(playing[0]==ID(4)); // highest child wins an empty parent channel
  lv_set(&r,NULL,"member",parent,lv_make(LV_MEMBER,ID(9)));
  call("updatestage",0,NULL);assert(playing[0]==ID(3)); // parent's own sound wins over its children
  dg_films_dispose(&d);
  puts("film sounds: two channels, priority, sustain, rewind, puppet, pause and disabled sound passed");
}
