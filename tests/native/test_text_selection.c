// Field hilite retains its owner/range and visibly selects through shared
// layout geometry. Portable inversion colors are not projector pixel evidence.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static uint32_t original[WC_WIDTH*WC_HEIGHT];
static const lv_movie_t code={.name="SELECTION.DXR"};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_text_style_t style={.font_name="Measured",.font_id=1,.size=12,.ascent=11,.line_height=14};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=3,
 .width=40,.height=28,.name="Choice",.text="ABC\rD",.text_style=&style};
static const dg_frame_t frame={0,0};
static const uint32_t palette[256]={0};
static const dg_movie_t movie={.id=1,.code=&code,.casts=casts,.cast_count=1,.members=&member,
 .member_count=1,.frames=&frame,.frame_count=1,.stage_color=0xffffff,.palette=palette};
static int layout(void *ctx,const char *text,int width,int height,const wc_text_style_t *font,int16_t *boxes,int max) {
 (void)ctx;(void)width;(void)height;(void)font;int x=0,y=0,n=0;
 for(;*text&&n<max;text++,n++) {
  memset(boxes+n*4,0,4*sizeof(*boxes));
  if(*text=='\r'||*text=='\n'){x=0;y+=14;continue;}
  boxes[n*4]=(int16_t)x;boxes[n*4+1]=(int16_t)y;boxes[n*4+2]=8;boxes[n*4+3]=14;x+=8;
 }
 return n;
}
static bool raster(void *ctx,const char *text,int width,int height,const wc_text_style_t *font,uint8_t *coverage) {
 int16_t boxes[64*4]={0};int n=layout(ctx,text,width,height,font,boxes,64);
 for(int i=0;i<n;i++)if(boxes[i*4+3]>0) {
  int x=boxes[i*4],y=boxes[i*4+1];
  if(x>=0&&x<width&&y>=0&&y<height)coverage[y*width+x]=255;
 }
 return true;
}
static void hilite(int first,int last) {
 lv_frame_t f={.movie=&code};
 lv_t args[]={lv_text(&values,"char",false),lv_num(first),lv_num(last),lv_make(LV_MEMBER,member.id)};
 lv_t range=lv_call(&values,&f,"text_range",4,args);
 (void)lv_call(&values,&f,"hilite_chunk",1,&range);assert(!values.failed);
 wc_render(&compositor,&director,0,0,false,0);assert(!values.failed);
}
static uint32_t pixel(int x,int y) {return compositor.scene[y*WC_WIDTH+x]&0xffffff;}
int main(void) {
 wc_init(&compositor,(wc_host_t){.text=raster,.layout=layout},1024);
 dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
 assert(dg_enter(&director,&movie,1,NULL,0));
 director.sprites[1]=(dg_sprite_t){.value={.member=member.id,.type=16,.x=10,.y=10,
  .width=40,.height=28,.blend=100},.visible=true};
 dg_sprite_changed(&director,1);wc_render(&compositor,&director,0,0,false,0);
 assert(!values.failed);memcpy(original,compositor.scene,sizeof(original));
 assert(pixel(10,10)==0&&pixel(18,10)==0&&pixel(20,11)==0xffffff);
 hilite(2,3);
 assert(director.hilite_start==1&&director.hilite_end==3);
 assert(pixel(10,10)==0&&pixel(18,10)==0xffffff&&pixel(20,11)==0);
 assert(pixel(49,11)==0xffffff&&pixel(10,24)==0);
 hilite(2,4);assert(pixel(49,11)==0&&pixel(10,24)==0);
 hilite(2147483647,2147483647);
 assert(director.hilite_start==5&&director.hilite_end==5);
 assert(!memcmp(original,compositor.scene,sizeof(original)));
 wc_release(&compositor);puts("text hilite: retained range, visible selection, selected RETURN and collapse restore passed");
}
