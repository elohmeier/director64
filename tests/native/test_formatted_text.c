// The compositor retains source formatting in stage pixels, snapshots and
// character positions, and applies whole-field edits without dropping runs.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static unsigned rich_calls,normal_calls,expected_size,expected_face=99,expected_align=99,expected_font;
static const uint32_t palette[256]={0};
static const lv_movie_t code={.name="RICH.DXR"};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_text_style_t styles[]={
 {.font_name="A",.font_id=1,.size=12,.ascent=11,.line_height=14},
 {.font_name="B",.font_id=2,.size=24,.ascent=21,.line_height=28}};
static const dg_text_format_t formats[]={
 {.offset=0,.rgb=0xff0000,.font=1,.size=12,.face=1,.align=1,.ascent=11,.line_height=14},
 {.offset=2,.rgb=0x0000ff,.font=2,.size=24,.face=2,.align=1,.ascent=21,.line_height=28}};
static const dg_member_t members[]={
 {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=3,.width=64,.height=40,.name="Mixed",.text="ABCD",
  .text_style=&styles[0],.text_formats=formats,.text_format_count=2},
 {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=3,.name="Other font",.text_style=&styles[1]}};
static const dg_frame_t frames[]={{0,0}};
static const dg_movie_t movie={.id=1,.code=&code,.tempo=30,.casts=casts,.cast_count=1,
 .members=members,.member_count=2,.frames=frames,.frame_count=1,.stage_color=0xffffff,.palette=palette};
static void check_formats(const dg_text_format_t *f,unsigned n) {
 assert(n==2&&f[0].offset==0&&f[1].offset==2&&f[0].rgb==0xff0000&&f[1].rgb==0x0000ff);
 assert(f[0].font==(expected_font?expected_font:1)&&f[1].font==2);
 assert(f[0].size==(expected_size?expected_size:12)&&f[1].size==(expected_size?expected_size:24));
 assert(f[0].face==(expected_face<99?expected_face:1)&&f[1].face==(expected_face<99?expected_face:2));
 assert(f[0].align==(expected_align<99?expected_align:1)&&f[1].align==(expected_align<99?expected_align:1));
}
static bool rich_text(void *ctx,const char *text,int width,int height,const dg_text_format_t *f,unsigned n,uint32_t *pixels) {
 (void)ctx;assert(!strcmp(text,"ABCD"));check_formats(f,n);rich_calls++;
 for(int y=0;y<height;y++)for(int x=0;x<width;x++)pixels[y*width+x]=x<width/2?0x800000ffu:0xffff0000u;
 return true;
}
static int rich_layout(void *ctx,const char *text,int width,int height,const dg_text_format_t *f,unsigned n,int16_t *boxes,int max) {
 (void)ctx;(void)text;(void)width;(void)height;check_formats(f,n);assert(max>=4);
 const int16_t placed[]={3,0,8,28,11,0,8,28,19,0,12,28,31,28,12,28};memcpy(boxes,placed,sizeof(placed));return 4;
}
static bool normal_text(void *ctx,const char *text,int width,int height,const wc_text_style_t *style,uint8_t *coverage) {
 (void)ctx;(void)text;assert(style&&style->face==0);normal_calls++;memset(coverage,255,(size_t)width*height);return true;
}
static int normal_layout(void *ctx,const char *text,int width,int height,const wc_text_style_t *style,int16_t *boxes,int max) {
 (void)ctx;(void)text;(void)width;(void)height;assert(style&&max>0);boxes[0]=7;boxes[1]=0;boxes[2]=8;boxes[3]=(int16_t)style->line_height;return 1;
}
static bool position(void *ctx,uint32_t member,int pos,int *x,int *y) {
 (void)ctx;return wc_text_position(&compositor,&director,dg_member(&director,member),"ABCD",64,pos,x,y);
}
static void draw(void) {director.visual_revision++;wc_render(&compositor,&director,0,0,false,0);assert(!values.failed);}
int main(void) {
 wc_init(&compositor,(wc_host_t){.text=normal_text,.layout=normal_layout,.rich_text=rich_text,.rich_layout=rich_layout},1024);
 dg_init(&director,&values,(dg_platform_t){.text_position=position},NULL,NULL,0,1);
 assert(dg_enter(&director,&movie,1,NULL,0));lv_t member=lv_make(LV_MEMBER,members[0].id);
 director.sprites[1]=(dg_sprite_t){.value={.member=members[0].id,.type=16,.width=64,.height=40,.blend=100},.visible=true};
 draw();assert(rich_calls==1&&compositor.pixels[40]==0xffff0000u);
 unsigned red=compositor.pixels[4];assert((red&255)==255&&(red>>8&255)>100&&(red>>16&255)>100);
 draw();assert(rich_calls==1);
 int x,y;assert(position(NULL,members[0].id,4,&x,&y)&&x==31&&y==55);
 assert(!strcmp(lv_cstr(&values,lv_at(&values,lv_get(&values,NULL,"fontstyle",member),1)),"bold"));
#if DG_D10
 dg_pixel_image_t image;assert(wc_read_image(&compositor,&director,members[0].id,&image));
 assert(image.pixels[4]==0x800000ffu&&image.pixels[40]==0xffff0000u);free(image.pixels);
#endif
 expected_size=18;lv_set(&values,NULL,"fontsize",member,lv_num(18));draw();
 expected_align=2;lv_set(&values,NULL,"alignment",member,lv_symbol(&values,"right"));draw();
 expected_face=0;lv_set(&values,NULL,"fontstyle",member,lv_list(&values,1,(lv_t[]){lv_symbol(&values,"plain")},false));draw();
 expected_font=2;lv_set(&values,NULL,"font",member,lv_text(&values,"B",false));draw();
 lv_set(&values,NULL,"text",member,lv_text(&values,"new",false));draw();assert(normal_calls>0);
 wc_release(&compositor);puts("rich text: source styles, RGBA/alpha, cache, geometry and whole-field edits passed");
}
