// Synthetic nested-movie pixels, independent from retail media and fonts.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static dg_runtime_t host;
static lv_runtime_t values;
static wc_t compositor;
static const uint32_t palette[256] = {0xffffff,0};
static const lv_movie_t host_code = {.name="HOST.DXR"}, child_code = {.name="CHILD.DXR"};
static const dg_cast_t host_cast = {.name="Internal",.file=1,.cast=1}, child_cast = {.name="Internal",.file=2,.cast=1};
static dg_member_t host_members[] = {
  {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=9,.width=4,.height=4,.linked_movie=2,
   .linked_flags=0x12,.linked_left=10,.linked_top=20},
  {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=8,.width=2,.height=2,.shape=1,.filled=1},
};
static const dg_member_t child_members[] = {
  {.id=DG_ID(2,1,1),.number=1,.cast=1,.type=8,.width=2,.height=2,.shape=1,.filled=1},
};
static const dg_delta_t host_deltas[] = {
  {.channel=6,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,1),.width=8,.height=8,
    .x=40,.y=40,.ink=0,.blend=100,.stretch=1,.fore=1}},
  {.channel=7,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,1),.width=8,.height=8,
    .x=80,.y=40,.ink=0,.blend=100,.stretch=1,.fore=1}},
  {.channel=8,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,2),.width=2,.height=2,
    .x=42,.y=42,.ink=0,.blend=100,.flags=16,.fore_rgb=0x00ff00}},
};
static const dg_delta_t child_deltas[] = {
  {.channel=6,.mask=DG_ALL,.value={.type=1,.member=DG_ID(2,1,1),.width=2,.height=2,
    .x=10,.y=20,.ink=0,.blend=100,.flags=16,.fore_rgb=0xff0000}},
  {.channel=6,.mask=DG_POSITION,.value={.x=12,.y=22}},
};
static const dg_frame_t host_frames[]={{0,3},{3,0}}, child_frames[]={{0,1},{1,1}};
static const dg_movie_t host_movie={.id=1,.code=&host_code,.casts=&host_cast,.cast_count=1,
  .members=host_members,.member_count=2,.tempo=60,.frame_count=2,.frames=host_frames,
  .deltas=host_deltas,.stage_color=0x0000ff,.palette=palette};
static const dg_movie_t child_movie={.id=2,.code=&child_code,.casts=&child_cast,.cast_count=1,
  .members=child_members,.member_count=1,.tempo=60,.frame_count=2,.frames=child_frames,
  .deltas=child_deltas,.stage_color=0x0000ff,.palette=palette};
static const dg_movie_t *find(void *ctx,const char *stem,unsigned file) {
  (void)ctx;(void)stem;return file==1?&host_movie:file==2?&child_movie:NULL;
}
static bool read_image(void *ctx,uint32_t member,dg_pixel_image_t *out) {
  (void)ctx;return wc_read_image(&compositor,values.context,member,out);
}
static lv_t call(const char *name,unsigned count,const lv_t *args) {
  lv_frame_t f={.movie=values.current};
  lv_t result=lv_call(&values,&f,name,count,args);
  if(values.failed)fprintf(stderr,"%s: %s\n",name,values.error);
  assert(!values.failed);return result;
}
static uint32_t pixel(unsigned x,unsigned y) {return compositor.pixels[y*WC_WIDTH+x]&0xffffff;}
static void render(void) {
  wc_render(&compositor,&host,0,0,false,0);
  if(values.failed)fprintf(stderr,"render: %s\n",values.error);
  assert(!values.failed);
}
int main(void) {
  wc_init(&compositor,(wc_host_t){0},4096);
  dg_init(&host,&values,(dg_platform_t){.find_movie=find,.read_image=read_image},NULL,NULL,0,1);
  assert(dg_enter(&host,&host_movie,1,NULL,0));
  assert(dg_tick(&host,0,0,false,DG_SERVICE_BUDGET));
  call("gotoandstop",2,(lv_t[]){lv_make(LV_SPRITE,1),lv_num(1)});
  call("gotoandstop",2,(lv_t[]){lv_make(LV_SPRITE,2),lv_num(2)});
  render();
  assert(pixel(40,40)==0xff && pixel(43,40)==0xff); // source origin and 2x scale
  assert(pixel(44,40)==0xff0000 && pixel(39,40)==0xff0000); // source and parent clipping
  assert(pixel(42,42)==0xff00); // later parent foreground covers nested content
  assert(pixel(80,40)==0xff0000 && pixel(84,44)==0xff); // independent playheads
  assert(!wc_render(&compositor,&host,0,0,false,0));

  dg_runtime_t *child=dg_linked_begin(&host,1);
  assert(child);
  child->visual_revision=host.visual_revision; // force every previous cache key to match
  dg_pixel_image_t snapshot;
  assert(wc_read_image(&compositor,child,0,&snapshot));
  assert(snapshot.width==800 && snapshot.height==600);
  assert((snapshot.pixels[20*800+10]&0xffffff)==0xff);
  free(snapshot.pixels);
  dg_linked_end(&host);
  render();
  assert(pixel(10,20)==0xff0000 && pixel(40,40)==0xff);

  child=dg_linked_begin(&host,1);
  call("setpixel",4,(lv_t[]){lv_make(LV_IMAGE,0),lv_num(10),lv_num(20),lv_make(LV_COLOR,0xffff00)});
  assert(dg_stage_image(child));
  dg_linked_end(&host);
  host.visual_revision++;
  render();
  assert(pixel(40,40)==0xffff && pixel(41,41)==0xffff); // stage pixels obey source origin too
  assert(pixel(42,42)==0xff00);
  call("gotoandstop",2,(lv_t[]){lv_make(LV_SPRITE,1),lv_num(2)});
  render();
  assert(pixel(40,40)==0xff0000 && pixel(44,44)==0xff);
  assert(pixel(84,44)==0xff);

  // Crop keeps the default 4x4 image size; center positions that image in
  // the resized sprite instead of anchoring it to the upper-left corner.
  host.sprites[3].visible=false;
  child=dg_linked_begin(&host,1);
  child->stage_color=0xffffff;child->visual_revision++;
  dg_linked_end(&host);
  host_members[0].linked_flags=0x10;
  host.visual_revision++;render();
  assert(pixel(40,40)==0xffffff && pixel(42,42)==0xff);
  assert(pixel(44,44)==0xff0000);
  host_members[0].linked_flags=0x11;
  host.visual_revision++;render();
  assert(pixel(40,40)==0xff0000 && pixel(42,42)==0xffffff && pixel(44,44)==0xff);
  host.sprites[1].value.width=host.sprites[1].value.height=2;
  dg_sprite_changed(&host,1);render();
  assert(pixel(40,40)==0xffffff && pixel(41,41)==0xff && pixel(42,42)==0xff0000);
  host_members[0].linked_flags=0x10;
  host.visual_revision++;render();
  assert(pixel(40,40)==0xffffff && pixel(41,41)==0xffffff);
  host_members[0].linked_flags=0x11;
  host.sprites[1].value.width=host.sprites[1].value.height=8;
  host.sprites[1].flip_h=host.sprites[1].flip_v=true;
  dg_sprite_changed(&host,1);render();
  assert(pixel(42,42)==0xff && pixel(44,44)==0xffffff && pixel(40,40)==0xff0000);
  dg_linked_dispose(&host);dg_images_dispose(&host);wc_release(&compositor);
  puts("linked compositor: source offset, scale, crop, center, flip, clip, ordering, separate playheads, snapshot ownership and stage mutation passed");
}
