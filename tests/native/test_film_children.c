// Synthetic film channels: authored frames, mutable empty slots and two parents.
#include "film_render.h"
#include <assert.h>
#include <stdio.h>
#define ID(n) DG_ID(1,1,n)
static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code = {.name="CHILDREN.DXR"};
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static const dg_frame_t score = {0};
static const dg_film_layer_t first[] = {
  {.channel=1,.value={.member=ID(3),.type=16,.x=12,.y=23,.width=4,.height=6,.blend=100,.ink=8}},
  {.channel=2,.value={.member=ID(99),.type=16,.x=20,.y=25,.width=8,.height=9,.blend=70,.stretch=1}},
  {.channel=300,.value={.member=ID(3),.type=16,.x=30,.y=26,.width=4,.height=6,.blend=100}},
};
static const dg_film_layer_t second[] = {
  {.channel=1,.value={.member=ID(3),.type=16,.x=17,.y=29,.width=4,.height=6,.blend=90,.ink=2}},
  {.channel=2,.value={.member=ID(99),.type=16,.x=22,.y=27,.width=10,.height=11,.blend=80,.stretch=1}},
};
static const dg_film_frame_t frames[] = {{3,first},{2,second}};
static const uint8_t keys1[] = {0,0,127,0,3,0,33,0,12,0,23,0,4,0,6,0,8,100,16,0,1,0,38,0,17,0,29,0,2,90};
static const uint8_t keys2[] = {0,0,127,1,99,0,33,0,20,0,25,0,8,0,9,0,0,70,16,0,0,1,0,1,0,62,0,22,0,27,0,10,0,11,0,0,80};
static const uint8_t keys300[] = {0,0,127,0,3,0,33,0,30,0,26,0,4,0,6,0,0,100,16,0,1,0,127,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const dg_film_track_t tracks[] = {{1,2,sizeof(keys1),keys1},{2,2,sizeof(keys2),keys2},{300,2,sizeof(keys300),keys300}};
static const dg_member_t members[] = {
  {.id=ID(1),.number=1,.cast=1,.type=2,.width=40,.height=40,.film_count=2,.film_loop=1,
   .film_track_count=3,.film_tracks=tracks,.film_left=10,.film_top=20},
  {.id=ID(2),.number=2,.cast=1,.type=2,.width=40,.height=40,.film_count=2,
   .film_frames=frames,.film_left=10,.film_top=20},
  {.id=ID(3),.number=3,.cast=1,.type=1,.width=4,.height=6,.reg_x=1,.reg_y=2},
  {.id=ID(4),.number=4,.cast=1,.type=1,.width=10,.height=12,.reg_x=2,.reg_y=3},
};
static const dg_movie_t movie = {.id=1,.code=&code,.cast_count=1,.casts=&cast,
  .member_count=4,.members=members,.frame_count=1,.frames=&score};
static lv_t child(unsigned parent,unsigned channel) {
  lv_t out=0;bool yield=false;
  assert(r.services.call(&r,"tell_child",2,(lv_t[]){lv_make(LV_SPRITE,parent),lv_num(channel)},&out,&yield));
  assert(!yield&&!r.failed);return out;
}
static int get(lv_t owner,const char *name) { return lv_integer(&r,lv_get(&r,NULL,name,owner)); }
static void put(lv_t owner,const char *name,lv_t value) {
  lv_set(&r,NULL,name,owner,value);
  if(r.failed)fprintf(stderr,"%s\n",r.error);
  assert(!r.failed);
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  // A negative ordinary channel must not be mistaken for an encoded child.
  assert(lv_id(lv_get(&r,NULL,"member",lv_make(LV_SPRITE,-1)))==0);
  assert(!r.failed);
  for(unsigned i=1;i<=2;i++) {
    d.sprites[i]=(dg_sprite_t){.value={.member=ID(1),.type=16,.blend=100},.visible=true};
    dg_sprite_changed(&d,i);
  }
  dg_films_present(&d,true);
  assert(dg_film_last_channel(&d,lv_make(LV_SPRITE,1))==300);
  assert(dg_film_last_channel(&d,lv_num(1))==300);
  for(unsigned n=1;n<=300;n++)put(child(1,n),"puppet",lv_num(0));
  assert(d.film_child_count==0);
  lv_t a=child(1,1), empty=child(1,2), high=child(1,300), other=child(2,1);
  assert(get(a,"loch")==12 && get(a,"locv")==23 && get(a,"width")==4);
  assert(lv_id(lv_get(&r,NULL,"member",empty))==ID(99));
  assert(get(high,"spritenum")==300 && high!=a);
  put(a,"blend",lv_num(50));
  d.mouse_hit_valid=true;
  put(empty,"member",lv_make(LV_MEMBER,ID(4)));
  assert(!d.mouse_hit_valid);
  assert(get(empty,"width")==8 && get(empty,"height")==9); // authored stretch retained
  assert(get(other,"blend")==100); // separate parent instances
  dg_film_frame_t merged;
  dg_film_layer_t *owned=dg_film_driven_frame(&d,1,&members[0],&merged);
  assert(owned && merged.count==3 && owned[0].value.member==ID(3));
  assert(owned[1].value.member==ID(4) && owned[2].channel==300);
  free(owned);
  d.sprites[1].film_frame=1;
  assert(get(high,"membernum")==0); // explicit blank key clears a channel
  assert(get(a,"loch")==17 && get(a,"locv")==29 && get(a,"ink")==2 && get(a,"blend")==50);
  assert(get(empty,"width")==10 && get(empty,"blend")==80); // untouched authored properties advance
  put(a,"loc",lv_point(&r,8,10));
  put(a,"member",lv_make(LV_MEMBER,ID(4)));
  assert(get(a,"width")==10 && get(a,"height")==12); // natural replacement size
  put(a,"width",lv_num(20));
  assert(get(a,"width")==20 && get(a,"height")==12);
  put(a,"rect",lv_rect(&r,3,4,23,28));
  assert(get(a,"left")==3 && get(a,"top")==4 && get(a,"width")==20 && get(a,"height")==24);
  put(a,"visible",lv_num(0));
  dg_film_layer_t layer;
  assert(dg_film_child_layer(&d,1,1,&layer) && !layer.value.type);
  put(a,"visible",lv_num(1));
  assert(dg_film_child_layer(&d,1,1,&layer) && layer.value.type==16);
  put(lv_make(LV_SPRITE,1),"member",lv_make(LV_MEMBER,ID(2)));
  assert(!dg_film_driven(&d,1));
  put(lv_make(LV_SPRITE,1),"member",lv_make(LV_MEMBER,ID(1)));
  assert(get(a,"blend")==100 && get(a,"loch")==12); // no resurrection after A -> B -> A
  assert(lv_id(lv_get(&r,NULL,"member",empty))==ID(99));
  assert(!dg_film_layer_sprite(&d,1,&first[1],&(dg_sprite_t){0})); // absent slot stays blank
  put(empty,"member",lv_make(LV_MEMBER,ID(4)));
  put(empty,"loc",lv_point(&r,2,3));
  assert(dg_film_child_layer(&d,1,2,&layer));
  assert(dg_film_layer_sprite(&d,1,&layer,&(dg_sprite_t){0}));
  lv_t result=0; bool yield=false;
  assert(r.services.call(&r,"puppetsprite",2,(lv_t[]){a,lv_num(1)},&result,&yield));
  assert(get(a,"puppet")==1);
  d.sprites[1].film_frame=1;
  assert(get(a,"loch")==12); // whole-channel puppet freezes authored properties
  assert(r.services.call(&r,"puppetsprite",2,(lv_t[]){a,lv_num(0)},&result,&yield));
  assert(get(a,"puppet")==0 && get(a,"loch")==17);
  d.sprites[1].film_frame=0;
  assert(r.services.call(&r,"puppetsprite",2,(lv_t[]){empty,lv_num(1)},&result,&yield));
  put(empty,"stretch",lv_num(0));
  put(empty,"member",lv_make(LV_MEMBER,ID(4)));
  assert(get(empty,"width")==10 && get(empty,"height")==12);
  dg_films_dispose(&d);
  puts("film children: authored merge, poses, placeholders, geometry, isolation and lifetime passed");
}
