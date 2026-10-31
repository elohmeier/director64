// Source-free live vector field feedback: resize an authored rectangle and
// change stroke/fill before and after placement without recoloring snapshots.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t cached,uncached;
static const lv_movie_t code={.name="VECTORSTYLE.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const double points[]={-6,-4,6,-4,6,4,-6,4};
static const dg_vector_shape_t shape={.vertex_count=4,.vertices=points,.stroke_width=2,
 .stroke=0x112233,.fill=0x334455,.closed=true,.filled=true};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=1,.source_xtra=2,
 .name="Field",.asset="must-not-read.fdi",.width=16,.height=12,.reg_x=8,.reg_y=6,.vector_shape=&shape};
static const dg_frame_t frame={0,0};
static const uint32_t palette[256]={[0]=0xffffff};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,.members=&member,
 .member_count=1,.frames=&frame,.frame_count=1,.palette=palette,.stage_color=0xffffff};
static uint32_t asset(void *ctx,const char *name,uint8_t *bytes,uint32_t cap) {
 (void)ctx;(void)name;(void)bytes;(void)cap;assert(!"edited vectors must use owned pixels");return 0;
}
static void render(void) {
 wc_render(&cached,&director,0,0,false,0);wc_render(&uncached,&director,0,0,false,0);
 assert(!values.failed&&!memcmp(cached.scene,uncached.scene,sizeof(cached.scene)));
}
static uint32_t pixel(int x,int y) {
 uint32_t p=cached.scene[y*WC_WIDTH+x];return ((p&255)<<16)|(p&0xff00)|((p>>16)&255);
}
static uint32_t color(lv_t owner,const char *name) {
 lv_t value=lv_get(&values,NULL,name,owner);assert(lv_type(value)==LV_COLOR);return (uint32_t)lv_integer(&values,value);
}
int main(void) {
 dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);assert(dg_enter(&director,&movie,1,NULL,0));
 wc_init(&cached,(wc_host_t){.read=asset},1024);wc_init(&uncached,(wc_host_t){.read=asset},1024);uncached.cache_disabled=true;
 director.sprites[1]=(dg_sprite_t){.value={.member=member.id,.type=1,.x=30,.y=30,.width=16,.height=12,.blend=100,.ink=8,.fore=255,.back=0},.visible=true};
 dg_sprite_changed(&director,1);lv_t owner=lv_make(LV_MEMBER,member.id);
 assert(color(owner,"fillcolor")==0x334455&&color(owner,"strokecolor")==0x112233);
 lv_set(&values,NULL,"strokecolor",owner,lv_make(LV_COLOR,0xff0000));render();
 assert(pixel(30,30)==0x334455&&pixel(24,30)==0xff0000);
 lv_t snapshot=lv_get(&values,NULL,"image",owner);values.roots[DG_SCRATCH_ROOT]=snapshot;
 lv_t vertices=lv_get(&values,NULL,"vertexlist",owner);assert(lv_count(&values,vertices)==4);
 const int xy[8]={-10,-5,10,-5,10,5,-10,5};
 for(unsigned i=0;i<4;i++)lv_set(&values,NULL,"vertex",lv_at(&values,vertices,i+1),lv_point(&values,xy[i*2],xy[i*2+1]));
 lv_set(&values,NULL,"vertexlist",owner,vertices);
 lv_set(&values,NULL,"fillcolor",owner,lv_make(LV_COLOR,0x43ae4a));render();
 const dg_pixel_image_t *image=dg_member_image(&director,member.id);
 assert(image&&image->width==24&&image->height==14&&image->reg_x==12&&image->reg_y==7);
 assert(!(image->pixels[0]>>24));
 assert(pixel(30,30)==0x43ae4a&&pixel(20,30)==0xff0000);
 assert(color(owner,"fillcolor")==0x43ae4a&&color(owner,"strokecolor")==0xff0000);
 lv_frame_t caller={.movie=&code};
 lv_t previous=lv_call(&values,&caller,"getpixel",3,(lv_t[]){snapshot,lv_num(8),lv_num(6)});
 assert((uint32_t)lv_integer(&values,previous)==0x334455);
 lv_set(&values,NULL,"strokecolor",owner,lv_make(LV_COLOR,0x43ae4a));render();
 assert(pixel(20,30)==0x43ae4a);
 lv_set(&values,NULL,"fillcolor",owner,lv_make(LV_COLOR,0xe85943));render();assert(pixel(30,30)==0xe85943);
 values.roots[DG_SCRATCH_ROOT]=(lv_t){0};assert(!values.failed);
 wc_release(&cached);wc_release(&uncached);dg_images_dispose(&director);
 puts("vector styles: authored resize, live stroke/fill, alpha, snapshots and cached pixels passed");
}
