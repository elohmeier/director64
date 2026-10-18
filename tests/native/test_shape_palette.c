#include "compositor.h"
#include "palette.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t d;
static lv_runtime_t r;
static wc_t c;
static uint8_t brush[545];
static uint32_t fallback[256];
static unsigned missing_palette_traces;
static void trace(void *ctx,const char *message) {
  (void)ctx;
  if(strstr(message,"SCORE_PALETTE_UNAVAILABLE")) {
    assert(strstr(message,"movie=PALETTE.DXR"));missing_palette_traces++;
  }
}
#if DG_MODERN
static const char *const textures[8]={"other.fdi","brush.fdi","other.fdi","other.fdi","other.fdi","other.fdi","other.fdi","other.fdi"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1,.shape_patterns=textures};
#else
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
#endif
static const lv_movie_t code={.name="PALETTE.DXR"};
static const uint32_t custom[256]={[249]=0x123456,[6]=0xfedcba};
static const dg_member_t members[]={
  {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=8,.name="shape",
   .shape=1,.filled=true,.pattern=58,.width=4,.height=4,.asset="brush.fdi"},
  {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=4,.name="custom",.palette_count=256,.palette=custom},
};
#define shape members[0]
static const dg_delta_t deltas[]={
  {.channel=5,.value={.member=0xffff}},
  {.channel=5,.value={.member=0xff9a}},
  {.channel=5,.value={.member=DG_ID(1,1,2)}},
  {.channel=5,.value={.member=DG_ID(1,1,1)}},
  {.channel=5,.value={.member=DG_ID(1,1,999)}},
};
static const dg_frame_t frames[]={{0,1},{1,1},{2,1},{3,1},{4,1}};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
  .members=members,.member_count=2,.frames=frames,.frame_count=5,.deltas=deltas,.tempo=30,.palette=fallback};
static uint32_t read_asset(void *ctx,const char *name,uint8_t *out,uint32_t capacity) {
  (void)ctx;
  if(out) {
    assert(capacity>=sizeof(brush));memcpy(out,brush,sizeof(brush));
    if(!strcmp(name,"other.fdi"))out[544]=6;
  }
  return sizeof(brush);
}
static void puppet(lv_t value) {
  lv_t result=0;bool yielded=false;
  assert(r.services.call(&r,"puppetPalette",1,&value,&result,&yielded));
  assert(!r.failed);
}
static void pixel(uint32_t expected) {
  wc_render(&c,&d,0,0,false,0);
  if(r.failed)fprintf(stderr,"palette: %s\n",r.error);
  assert(!r.failed);
  uint32_t actual=c.pixels[20*WC_WIDTH+20]&0xffffffu;
  if(actual!=expected)fprintf(stderr,"palette pixel actual=%06x expected=%06x pattern=%u active=%06x\n",(unsigned)actual,(unsigned)expected,dg_member_pattern(&d,&shape),(unsigned)dg_stage_palette(&d)[249]);
  assert(actual==expected);
}
int main(void) {
  dg_system_palette(fallback,true);
  memcpy(brush,"FDIC",4);
  brush[4]=0;brush[5]=0;brush[6]=2;brush[7]=33;
  brush[9]=brush[11]=1;brush[16]=4;brush[18]=brush[19]=8;brush[20]=1;
  brush[544]=249;
  wc_init(&c,(wc_host_t){.read=read_asset},4096);
  dg_init(&d,&r,(dg_platform_t){.trace=trace},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  d.sprites[1]=(dg_sprite_t){.value={.type=16,.member=shape.id,.x=20,.y=20,.width=4,.height=4,
    .ink=0,.blend=100,.thickness=DG_HAS_BLEND,.fore=6},.visible=true,.stretch=true};
  dg_sprite_changed(&d,1);
  pixel(0x888888); // Same slot in the original Mac palette.
  puppet(lv_text(&r,"System - Win",false));pixel(0x808000);
  puppet(lv_text(&r,"System - Mac",false));
  assert(dg_seek(&d,2));pixel(0x888888); // Puppet owns palette despite score.
  puppet(lv_num(0));pixel(0x808000); // Release restores current score command.
  lv_t member=lv_make(LV_MEMBER,(int32_t)shape.id);
#if DG_MODERN
  lv_set(&r,NULL,"pattern",member,lv_num(57));pixel(0xff);
  assert(lv_integer(&r,lv_get(&r,NULL,"pattern",member))==57);
#endif
  lv_set(&r,NULL,"pattern",member,lv_num(58));pixel(0x808000);
  puppet(lv_text(&r,"Grayscale",false));pixel(0x060606);
  puppet(lv_make(LV_MEMBER,(int32_t)DG_ID(1,1,2)));pixel(0x563412);
  puppet(lv_num(0));pixel(0x808000);
  puppet(lv_text(&r,"custom",false));pixel(0x563412);
  puppet(lv_num(0));pixel(0x808000);
  assert(dg_seek(&d,3));pixel(0x563412);
  uint32_t revision=d.visual_revision;
  assert(dg_seek(&d,4));pixel(0x563412);
  assert(dg_seek(&d,5));pixel(0x563412);
  assert(missing_palette_traces==2 && d.visual_revision==revision);
  lv_set(&r,NULL,"pattern",member,lv_num(1));
#if DG_FAMILY_D7 || (defined(DIRECTOR64_D85) && DIRECTOR64_D85)
  pixel(0xbddeff); // This profile quantizes nontexture shape colors to555.
#else
  pixel(0xbadcfe);
#endif
#if DG_FAMILY_D7
  lv_t sprite=lv_make(LV_SPRITE,1);
  lv_set(&r,NULL,"color",sprite,lv_num(6));
  assert(r.failed && r.script_error && strstr(r.error,"Color expected"));
  assert(lv_recover_script(&r));
  const uint32_t inputs[]={0x123456,0x808080,0xa6caf0,0x996633,0xff1108,0x010203};
  const uint32_t expected[]={0x523110,0x848484,0xf7cea5,0x31639c,0x0810ff,0x000000};
  for(unsigned i=0;i<6;i++) {
    lv_set(&r,NULL,"color",sprite,lv_make(LV_COLOR,(int32_t)inputs[i]));pixel(expected[i]);
  }
#endif
  lv_set(&r,NULL,"pattern",member,lv_num(65));assert(r.failed);
  wc_release(&c);
  puts("Live score/puppet palette indices and per-cast mutable shape patterns passed");
}
