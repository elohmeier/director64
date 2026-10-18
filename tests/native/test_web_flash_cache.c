// Source-free full-compositor cache regression for interactive Flash.
#include "director.h"
#include "flash_runtime.h"
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d; static lv_runtime_t v; static wc_t c;
static const lv_movie_t code={.name="SYNTHETIC.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t score[]={{0,0}};
static const uint32_t palette[256]={0xffffff,0};
static const dg_film_layer_t root_layer={.channel=1,.clip_id=7,.name="child",.placement_generation=1,.affine=true,.matrix={65536,0,0,65536,655360,655360}};
static const dg_film_frame_t root_frames[]={{1,&root_layer}};
static const dg_film_layer_t red={.channel=1,.asset="red",.value={.width=8,.height=8}};
static const dg_film_layer_t blue={.channel=1,.asset="blue",.value={.width=8,.height=8}};
static const dg_film_frame_t child_frames[]={{1,&red},{1,&blue}};
static const dg_flash_clip_t clips[]={
 {.id=0,.frame_count=1,.local_bounds={0,0,64*65536,64*65536}},
 {.id=7,.frame_count=2,.frames=child_frames,.local_bounds={0,0,8*65536,8*65536}}};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,.width=64,.height=64,
 .film_count=1,.film_loop=1,.source_xtra=1,.film_frames=root_frames,.flash_version=6,.flash_clip_count=2,.flash_clips=clips};
static const dg_movie_t movie={.id=1,.code=&code,.cast_count=1,.casts=&cast,.member_count=1,.members=&member,.frame_count=1,.frames=score,.palette=palette};
static uint32_t read_asset(void *ctx,const char *name,uint8_t *out,uint32_t cap){
 (void)ctx;unsigned n=160;if(out){assert(cap>=n);memset(out,0,n);memcpy(out,"FDI1",4);out[7]=n;out[9]=out[11]=8;
 unsigned color=!strcmp(name,"red")?0xf801:0x003f;for(unsigned i=32;i<n;i+=2){out[i]=color>>8;out[i+1]=color;}}return n;}
int main(void){
 dg_init(&d,&v,(dg_platform_t){0},NULL,NULL,0,1);assert(dg_enter(&d,&movie,1,NULL,0));
 d.sprites[1]=(dg_sprite_t){.value={.member=member.id,.type=16,.width=64,.height=64,.blend=100,.ink=8,.flags=48,.back_rgb=0xffffff},.visible=true,.stretch=true};
 dg_sprite_changed(&d,1);dg_flash_sync(&d);assert(dg_flash_service(&d,100000));
 wc_init(&c,(wc_host_t){.read=read_asset},100000);
 assert(wc_render(&c,&d,0,0,false,0));assert(!v.failed);
 unsigned before=d.visual_revision;uint32_t old=c.pixels[10*WC_WIDTH+10];
 dg_flash_instance_t *root=dg_flash_root(&d,1),*child=dg_flash_child(root,1);assert(root&&child);
 dg_avm1_set(dg_flash_vm(child),dg_flash_object(child),"_x",lv_num(20));
 bool rendered=wc_render(&c,&d,0,0,false,0);
 assert(rendered&&d.visual_revision>before&&c.pixels[10*WC_WIDTH+10]!=old);
 assert(c.pixels[10*WC_WIDTH+20]==old);
 assert(!wc_render(&c,&d,0,0,false,0));
 dg_flash_play(&d,1,false);before=d.visual_revision;dg_flash_advance(&d,1,0);rendered=wc_render(&c,&d,0,0,false,0);
 assert(rendered&&d.visual_revision>before&&c.pixels[10*WC_WIDTH+20]!=old);
 wc_release(&c);dg_flash_dispose(&d);dg_films_dispose(&d);dg_images_dispose(&d);
 puts("Flash compositor: child edits and paused-root animation invalidate cached pixels");return 0;
}
