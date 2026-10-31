// Source-free loaded/preload residency, separate from whether a cast record
// exists. Native film score metadata is eager; bitmap decode is bounded/lazy.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static unsigned reads;
static uint8_t pixels[40];
static const lv_movie_t code={.name="LOAD.DXR"};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_member_t members[]={
 {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=1,.width=4,.height=1,.name="Bitmap",.asset="sample.fdi"},
 {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=2,.name="Native film"},
 {.id=DG_ID(1,1,3),.number=3,.cast=1,.type=1,.width=4,.height=1,.name="Missing",.asset="missing.fdi"}};
static const dg_frame_t frames[]={{0,0}};
static const dg_movie_t movie={.id=1,.code=&code,.tempo=30,.casts=casts,.cast_count=1,
 .members=members,.member_count=3,.frames=frames,.frame_count=1};
static uint32_t read_asset(void *ctx,const char *name,uint8_t *out,uint32_t cap) {
 (void)ctx;reads++;if(strcmp(name,"sample.fdi"))return 0;
 if(out){assert(cap>=sizeof(pixels));memcpy(out,pixels,sizeof(pixels));}return sizeof(pixels);
}
static bool resident(void *ctx,uint32_t member,bool load) {
 (void)ctx;return wc_member_resident(&compositor,&director,member,load);
}
static int loaded(lv_t member) {return lv_integer(&values,lv_get(&values,NULL,"loaded",member));}
static int preload(lv_t member) {
 lv_frame_t frame={.movie=&code};return lv_integer(&values,lv_call(&values,&frame,"preload",1,&member));
}
int main(void) {
 memcpy(pixels,"FDI1",4);pixels[7]=40;pixels[9]=4;pixels[11]=1;
 for(unsigned i=32;i<40;i+=2){pixels[i]=0xf8;pixels[i+1]=1;}
 wc_init(&compositor,(wc_host_t){.read=read_asset},1024);
 dg_init(&director,&values,(dg_platform_t){.member_resident=resident},NULL,NULL,0,1);
 assert(dg_enter(&director,&movie,1,NULL,0));
 lv_t bitmap=lv_make(LV_MEMBER,members[0].id),film=lv_make(LV_MEMBER,members[1].id);
 assert(loaded(film)==1&&loaded(bitmap)==0&&reads==0);
 assert(preload(film)==2&&reads==0);
 assert(preload(bitmap)==1&&reads>0&&loaded(bitmap)==1);
 unsigned previous=reads;assert(loaded(bitmap)==1&&preload(bitmap)==1&&reads==previous);
 assert(lv_integer(&values,values.the_result)==1);
 wc_release(&compositor);assert(loaded(bitmap)==0);
 assert(!values.failed);
 assert(preload(lv_make(LV_MEMBER,members[2].id))==0);
 assert(values.failed&&strstr(values.error,"missing.fdi"));
 wc_release(&compositor);
 puts("member loaded: eager film records, lazy bitmap cache, preload, eviction and missing asset diagnostics passed");
}
