// Synthetic pixel regression for a recovered D7 requirement: a cropped
// multi-bit sky layer uses Blend ink and has a transparent exterior cutout.
// No retail pixels are embedded. Original-projector D7 comparison is pending.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static uint8_t image[48];
static const lv_movie_t code = {.name="BLEND.DXR"};
static const dg_cast_t casts[] = {{.name="Internal", .file=1, .cast=1}};
static const dg_frame_t frames[] = {{0, 0}};
static const dg_member_t members[] = {
  {.id=DG_ID(1,1,1), .number=1, .cast=1, .type=1, .name="synthetic-sky",
   .width=4, .height=2, .asset="sky.fdi"},
};
static const dg_movie_t movie = {
  .code=&code, .id=1, .tempo=30, .cast_count=1, .member_count=1, .frame_count=1,
  .casts=casts, .members=members, .frames=frames, .stage_color=0x00ff00,
};
static uint32_t read_asset(void *context, const char *name, uint8_t *out, uint32_t cap) {
  (void)context;
  if (strcmp(name,"sky.fdi")) return 0;
  if (out) {assert(cap>=sizeof(image)); memcpy(out,image,sizeof(image));}
  return sizeof(image);
}
static void word(uint8_t *p, unsigned value) {p[0]=(uint8_t)(value>>8);p[1]=(uint8_t)value;}
static uint32_t pixel(unsigned x, unsigned y) {
  return compositor.pixels[y*WC_WIDTH+x]&0xffffffu;
}
static void draw(unsigned depth, unsigned ink, unsigned opacity) {
  wc_release(&compositor);
  memset(image,0,sizeof(image));
  memcpy(image,"FDI1",4);image[7]=sizeof(image);
  word(image+8,4);word(image+10,2);image[19]=(uint8_t)depth;
  const unsigned colors[]={0xf801,0xf801,0xffff,0xf801,0xfffe,0xfffe,0xf801,0xf801};
  for (unsigned i=0;i<8;i++)word(image+32+i*2,colors[i]);
  wc_init(&compositor,(wc_host_t){.read=read_asset},4096);
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&director,&movie,1,NULL,0));
  director.sprites[1]=(dg_sprite_t){
    .value={.member=DG_ID(1,1,1),.type=16,.x=20,.y=20,.width=4,.height=2,
            .ink=ink,.blend=opacity,.thickness=DG_HAS_BLEND,.flags=48,.loc_z=1},
    .stretch=true,.visible=true};
  dg_sprite_changed(&director,1);
  wc_render(&compositor,&director,0,0,false,0);
  assert(!values.failed);
}
int main(void) {
  draw(8,32,100);
  assert(pixel(20,21)==0x00ff00); // exterior matte reveals the green stage
  assert(pixel(22,20)==0xffffff); // opaque white remains; this is not a white key
  assert(pixel(20,20)==0x0000ff); // colored portion remains visible
  draw(8,32,50);
  assert(pixel(20,21)==0x00ff00);
  unsigned mixed=pixel(20,20);
  assert((mixed&255)>0 && (mixed&255)<255 && ((mixed>>8)&255)>0);
  draw(1,32,100);
  assert(pixel(20,21)==0xffffff); // one-bit Blend keeps its whole rectangle
  draw(8,0,100);
  assert(pixel(20,21)==0xffffff); // full-opacity Copy remains rectangular
  wc_release(&compositor);
  puts("D7 multi-bit Blend preserves matte, opaque interiors and one-bit/Copy controls");
}
