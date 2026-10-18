// Source-free temporal recursion, qualified against the Birthday original
// projector (8.5.1r104 engine, MX9 launcher): first presentation holds a new
// root, nested films start one pose later, seek retains independent children.
#include "film_render.h"
#include <assert.h>
#include <stdio.h>
#define ID(n) DG_ID(1,1,n)
static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code={.name="FILM-CLOCK.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t score={0};
// Frame0: member1,type16. Frame3: empty. Recursion therefore terminates
// in a later pose of the same member, despite a cyclic member graph.
static const uint8_t data[]={0,0,65,0,1,0,33,0,16,0,3,0,65,0,0,0,0,0,0,0};
static const dg_film_track_t track={1,2,sizeof(data),data};
static const dg_film_layer_t sparse_layer={.channel=DG_SPRITES-1,
 .value={.member=ID(1),.type=16,.blend=100}};
static const dg_film_frame_t sparse_frames[]={{1,&sparse_layer},{0,NULL},{1,&sparse_layer}};
static const dg_film_layer_t moving_layers[]={
 {.channel=1,.value={.member=ID(2),.type=16,.blend=100}},
 {.channel=1,.value={.member=ID(2),.type=16,.blend=100,.x=1}},
};
static const dg_film_frame_t moving_frames[]={{1,&moving_layers[0]},{1,&moving_layers[1]}};
static const dg_film_layer_t animated_child={.channel=1,
 .value={.member=ID(5),.type=16,.blend=100}};
static const dg_film_frame_t animated_parent={1,&animated_child};
static const dg_film_layer_t ended_child={.channel=1,
 .value={.member=ID(4),.type=16,.blend=100}};
static const dg_film_frame_t ended_parent={1,&ended_child};
static const dg_film_layer_t animated_grandchild={.channel=1,
 .value={.member=ID(6),.type=16,.blend=100}};
static const dg_film_frame_t animated_grandparent={1,&animated_grandchild};
static const dg_member_t members[]={
  {.id=ID(1),.number=1,.cast=1,.type=2,.width=8,.height=8,.film_count=4,.film_loop=1,
   .film_track_count=1,.film_tracks=&track},
  {.id=ID(2),.number=2,.cast=1,.type=1,.width=8,.height=8},
  {.id=ID(3),.number=3,.cast=1,.type=2,.width=8,.height=8,.film_count=3,.film_loop=1,
   .film_frames=sparse_frames},
  {.id=ID(4),.number=4,.cast=1,.type=2,.width=8,.height=8,.film_count=2,
   .film_frames=moving_frames},
  {.id=ID(5),.number=5,.cast=1,.type=2,.width=8,.height=8,.film_count=2,.film_loop=1,
   .film_frames=moving_frames},
  {.id=ID(6),.number=6,.cast=1,.type=2,.width=8,.height=8,.film_count=1,.film_loop=1,
   .film_frames=&animated_parent,.film_flags=8},
  {.id=ID(7),.number=7,.cast=1,.type=2,.width=8,.height=8,.film_count=1,
   .film_frames=&ended_parent},
  {.id=ID(8),.number=8,.cast=1,.type=2,.width=8,.height=8,.film_count=1,
   .film_frames=&animated_grandparent},
};
static const dg_movie_t movie={.id=1,.code=&code,.cast_count=1,.casts=&cast,
 .member_count=sizeof(members)/sizeof(members[0]),.members=members,.frame_count=1,.frames=&score};
static void call(const char *name,unsigned argc,lv_t *args) {
  lv_t out=0;bool yield=false;
  assert(r.services.call(&r,name,argc,args,&out,&yield));
  if(r.failed)fprintf(stderr,"%s\n",r.error);
  assert(!r.failed&&!yield);
}
static void stable_present(void) {
  unsigned revision=d.visual_revision,order=d.order_serial;
  assert(!d.stage_count);
  dg_films_present(&d,true);
  assert(!d.stage_count&&d.visual_revision==revision&&d.order_serial==order);
  dg_update_stage(&d); // Sound selection remains serviced on an unchanged stage.
  assert(d.visual_revision==revision);
}
static void test_visual_revision(lv_t root,lv_t other) {
  lv_set(&r,NULL,"member",other,lv_make(LV_MEMBER,0));
  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(4)));
  call("updatestage",0,NULL);
  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,root,false)==2);
  for(unsigned i=0;i<3;i++)stable_present(); // Nonlooping root has ended.

  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(5)));
  call("updatestage",0,NULL);
  call("stop",1,&root);
  unsigned revision=d.visual_revision;
  dg_film_frame_t frame;
  dg_film_layer_t *owned=dg_film_frame_resolve(&d,1,&d.sprites[1],&members[4],&frame);
  assert(owned);free(owned);
  assert(dg_film_frame(&d,root,false)==1&&d.visual_revision==revision);
  for(unsigned i=0;i<3;i++)stable_present(); // Paused looping root is stable too.

  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(6)));
  // Sound selection allocates a root without presenting it. An early local
  // edit still establishes nested tell identity, while getters stay blank.
  unsigned sound_pose,sound_source;
  assert(!dg_film_sound(&d,1,0,&sound_pose,&sound_source)&&sound_source);
  lv_t early=dg_film_tell(&d,root,1);
  lv_set(&r,NULL,"loch",early,lv_num(7));
  lv_t early_leaf=dg_film_tell(&d,early,1);
  assert((uint32_t)lv_id(early_leaf)&DG_CHILD_SPRITE);
  assert(!lv_id(lv_get(&r,NULL,"member",early)));
  assert(!lv_id(lv_get(&r,NULL,"member",early_leaf)));
  call("updatestage",0,NULL);
  call("stop",1,&root);
  lv_t child=dg_film_tell(&d,root,1);
  assert(dg_film_frame(&d,child,false)==2);
  lv_t leaf=dg_film_tell(&d,child,1);
  // A drawAnimation-style batch changes local geometry and appearance at
  // several depths without advancing or replacing either live playhead.
  for(unsigned i=0;i<3;i++) {
    lv_set(&r,NULL,"loch",child,lv_num(20+i));
    lv_set(&r,NULL,"blend",child,lv_num(70+i));
    lv_set(&r,NULL,"width",child,lv_num(10+i));
    lv_set(&r,NULL,"loc",leaf,lv_point(&r,30+i,40+i));
    lv_set(&r,NULL,"ink",leaf,lv_num(8));
    lv_set(&r,NULL,"height",leaf,lv_num(12+i));
    lv_set(&r,NULL,"color",leaf,lv_make(LV_COLOR,0x123456));
    assert(!r.failed&&dg_film_frame(&d,root,false)==1&&dg_film_frame(&d,child,false)==2);
    assert(lv_integer(&r,lv_get(&r,NULL,"loch",child))==(int)(20+i));
    assert(lv_integer(&r,lv_get(&r,NULL,"blend",child))==(int)(70+i));
    assert(lv_integer(&r,lv_get(&r,NULL,"locv",leaf))==(int)(40+i));
    assert(lv_integer(&r,lv_get(&r,NULL,"height",leaf))==(int)(12+i));
  }
  // Membership setters still synchronize immediately; puppet release restores
  // the recorded bitmap and retires the temporary nested film.
  lv_set(&r,NULL,"membernum",leaf,lv_num(4));
  assert(dg_film_frame(&d,leaf,false)==2);
  lv_set(&r,NULL,"castnum",leaf,lv_num(2));
  assert(dg_film_frame(&d,leaf,false)==0);
  lv_set(&r,NULL,"member",leaf,lv_make(LV_MEMBER,ID(4)));
  assert(dg_film_frame(&d,leaf,false)==2);
  lv_set(&r,NULL,"puppet",leaf,lv_num(0));
  assert(dg_film_frame(&d,leaf,false)==0);
  assert(lv_id(lv_get(&r,NULL,"member",leaf))==ID(2));
  dg_update_stage(&d);
  revision=d.visual_revision;
  static dg_runtime_t host,outer;
  host.linked_parent=&outer;d.linked_parent=&host;
  host.mouse_hit_valid=outer.mouse_hit_valid=true;
  unsigned host_revision=host.visual_revision,outer_revision=outer.visual_revision;
  dg_films_present(&d,true);
  assert(dg_film_frame(&d,root,false)==1&&dg_film_frame(&d,child,false)==1);
  assert(d.visual_revision>revision&&d.stage_dirty[1]);
  assert(host.visual_revision>host_revision&&outer.visual_revision>outer_revision);
  assert(!host.mouse_hit_valid&&!outer.mouse_hit_valid);
  d.linked_parent=NULL;dg_update_stage(&d);
  // Seeking the unchanged root advances its independently running child.
  revision=d.visual_revision;
  assert(dg_film_go(&d,root,1));
  assert(dg_film_frame(&d,child,false)==2&&d.visual_revision>revision);
  dg_update_stage(&d);

  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(7)));
  call("updatestage",0,NULL);
  for(unsigned i=0;i<3;i++)stable_present(); // Root and nested child both ended.

  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(8)));
  call("updatestage",0,NULL);
  child=dg_film_tell(&d,root,1);
  lv_t grand=dg_film_tell(&d,child,1);
  unsigned pose=dg_film_frame(&d,grand,false);
  revision=d.visual_revision;
  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,root,false)==1&&dg_film_frame(&d,child,false)==1);
  assert(dg_film_frame(&d,grand,false)!=pose&&d.visual_revision>revision);
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);assert(dg_enter(&d,&movie,1,NULL,0));
  lv_t root=lv_make(LV_SPRITE,1),other=lv_make(LV_SPRITE,2);
  for(unsigned ch=1;ch<=2;ch++) {
    d.sprites[ch]=(dg_sprite_t){.value={.member=ID(1),.type=16,.blend=100},.visible=true};
    dg_sprite_changed(&d,ch);
  }
  lv_t child=dg_film_tell(&d,root,1);
  assert(lv_id(lv_get(&r,NULL,"member",child))==0); // no child before the first presentation
  unsigned before_prepare=d.visual_revision;
  dg_film_frame_t prepared;
  dg_film_layer_t *prepaint=dg_film_frame_resolve(&d,1,&d.sprites[1],&members[0],&prepared);
  assert(prepaint);free(prepaint);
  assert(d.visual_revision==before_prepare);
  assert(lv_id(lv_get(&r,NULL,"member",child))==0);
  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,root,false)==1&&dg_film_frame(&d,child,false)==2);
  lv_t grand=dg_film_tell(&d,child,1),great=dg_film_tell(&d,grand,1);
  assert(dg_film_frame(&d,grand,false)==3&&dg_film_frame(&d,great,false)==4);
  assert(lv_id(lv_get(&r,NULL,"member",dg_film_tell(&d,great,1)))==0);
  for(unsigned i=0;i<10;i++)assert(dg_film_frame(&d,grand,false)==3); // getters are inert
  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,root,false)==2&&dg_film_frame(&d,child,false)==3);
  assert(dg_film_frame(&d,grand,false)==4);
  assert(dg_film_go(&d,root,1));
  assert(dg_film_frame(&d,root,false)==1&&dg_film_frame(&d,child,false)==4);
  assert(dg_film_frame(&d,other,false)==2); // distinct parents
  dg_film_layer_t layer;
  assert(dg_film_child_layer(&d,1,1,&layer)&&layer.film_frame==3);
  dg_film_frame_t frame;
  dg_film_layer_t *owned=dg_film_frame_resolve(&d,1,&d.sprites[1],&members[0],&frame);
  assert(owned&&frame.count==1&&owned[0].film_frame==3);free(owned);
  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(2)));
  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(1)));
  assert(!r.failed);
  child=dg_film_tell(&d,root,1);
  assert(lv_id(lv_get(&r,NULL,"member",child))==0);
  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,root,false)==1&&dg_film_frame(&d,child,false)==2);
  // Packed cursor replay after a backward seek restores the active member.
  assert(dg_film_go(&d,root,4));
  assert(lv_id(lv_get(&r,NULL,"member",dg_film_tell(&d,root,1)))==0);
  assert(dg_film_go(&d,root,1));
  assert(lv_id(lv_get(&r,NULL,"member",dg_film_tell(&d,root,1)))==ID(1));
  // Sparse expanded frames retire an omitted high-numbered child, then
  // create a fresh instance when it reappears. Overrides can introduce a
  // second unrecorded channel; the resolved union stays sorted and unique.
  lv_set(&r,NULL,"member",root,lv_make(LV_MEMBER,ID(3)));
  call("updatestage",0,NULL);
  lv_t high=dg_film_tell(&d,root,DG_SPRITES-1);
  assert(dg_film_frame(&d,high,false)==2);
  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,high,false)==0);
  call("updatestage",0,NULL);
  assert(dg_film_frame(&d,high,false)==4);
  lv_set(&r,NULL,"member",high,lv_make(LV_MEMBER,ID(2)));
  assert(dg_film_frame(&d,high,false)==0);
  lv_t added=dg_film_tell(&d,root,DG_SPRITES-2);
  lv_set(&r,NULL,"member",added,lv_make(LV_MEMBER,ID(1)));
  assert(dg_film_frame(&d,added,false)==4);
  owned=dg_film_frame_resolve(&d,1,&d.sprites[1],&members[2],&frame);
  assert(owned && frame.count==2 && owned[0].channel==DG_SPRITES-2 &&
      owned[0].value.member==ID(1) && owned[1].channel==DG_SPRITES-1 &&
      owned[1].value.member==ID(2));
  free(owned);assert(!r.failed);
  test_visual_revision(root,other);
  dg_films_dispose(&d);
  puts("native film playback: temporal recursion, clocks, seek, lifetime and subtree cache invalidation passed");
}
