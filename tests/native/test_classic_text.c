// Classic field structure observed in original8.5 under Wine. Measured font
// values are supplied by this deterministic host; no retail glyphs are used.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static unsigned queries;
static const lv_movie_t code={.name="CLASSIC.DXR"};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_text_style_t style={.font_name="Measured",.font_id=1,.size=12};
static const dg_member_t members[]={{.id=DG_ID(1,1,1),.number=1,.cast=1,.type=3,
 .text_box_present=true,.text_box_type=1,.text_word_wrap=true,
 .width=100,.height=40,.name="Classic",.text="ABCD\rEF",.text_style=&style}};
static const dg_frame_t frames[]={{0,0}};
static const dg_movie_t movie={.id=1,.code=&code,.casts=casts,.cast_count=1,
 .members=members,.member_count=1,.frames=frames,.frame_count=1};
static int rich_layout(void *ctx,const char *text,int width,int height,
 const dg_text_format_t *formats,unsigned count,int16_t *boxes,int max) {
 (void)ctx;(void)height;assert(count>=1&&formats[0].font==1);
 int pitch=formats[0].line_height>0?formats[0].line_height:15;
 int ascent=formats[0].ascent>0?formats[0].ascent:11;
 int cell=(formats[0].layout_flags&DG_TEXT_CLASSIC_FIELD)?ascent+3:pitch;
 int n=0,x=0,y=0;
 for(;*text&&n<max;text++,n++) {
  memset(boxes+n*4,0,4*sizeof(*boxes));
  if(*text=='\r'||*text=='\n'){x=0;y+=pitch;continue;}
  if(x+8>width&&x&&!(formats[0].layout_flags&DG_TEXT_NO_WRAP)){x=0;y+=pitch;}
  boxes[n*4]=(int16_t)x;boxes[n*4+1]=(int16_t)y;
  boxes[n*4+2]=8;boxes[n*4+3]=(int16_t)cell;x+=8;
 }
 return n;
}
static int layout(void *ctx,const char *text,int width,int height,
 const wc_text_style_t *font,int16_t *boxes,int max) {
 dg_text_format_t f={.font=(uint8_t)font->font};
 return rich_layout(ctx,text,width,height,&f,1,boxes,max);
}
static int measure(void *ctx,uint32_t id,unsigned width) {
 (void)ctx;queries++;assert(queries<100);
 const char *text=members[0].text;
 for(unsigned i=0;i<director.field_count;i++)if(director.field_members[i]==id)
  text=lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT+i]);
 return wc_text_height(&compositor,&director,&members[0],text,(int)width);
}
static void at(int position,int expected) {
 int x,y;const char *text=lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT]);
 assert(wc_text_position(&compositor,&director,&members[0],text,100,position,&x,&y));
 assert(x==0&&y==expected&&!values.failed);
}
int main(void) {
 wc_init(&compositor,(wc_host_t){.layout=layout,.rich_layout=rich_layout},1024);
 dg_init(&director,&values,(dg_platform_t){.text_height=measure},NULL,NULL,0,1);
 assert(dg_enter(&director,&movie,1,NULL,0));lv_t member=lv_make(LV_MEMBER,members[0].id);
 assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"type",member)),"field"));
 assert(lv_type(lv_get(&values,NULL,"fontstyle",member))==LV_STRING);
 assert(lv_type(lv_get(&values,NULL,"alignment",member))==LV_STRING);
 lv_set(&values,NULL,"fontstyle",member,lv_text(&values,"bold, italic",false));
 assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"fontstyle",member)),"bold,italic"));
 lv_set(&values,NULL,"fontstyle",member,lv_text(&values,"plain",false));
 assert(lv_integer(&values,lv_get(&values,NULL,"lineheight",member))==15);
 lv_set(&values,NULL,"boxtype",member,lv_symbol(&values,"adjust"));
 assert(lv_integer(&values,lv_get(&values,NULL,"height",member))==30);
 at(1,13);at(6,28);
 lv_set(&values,NULL,"lineheight",member,lv_num(32));
 assert(lv_integer(&values,lv_get(&values,NULL,"lineheight",member))==32);
 assert(lv_integer(&values,lv_get(&values,NULL,"height",member))==64);
 at(1,13);at(6,45);
 lv_set(&values,NULL,"text",member,lv_text(&values,"ABCD\rEF",false));
 at(1,13);at(6,45);
 lv_set(&values,NULL,"lineheight",member,lv_num(0));
 assert(lv_integer(&values,lv_get(&values,NULL,"lineheight",member))==15);
 assert(lv_integer(&values,lv_get(&values,NULL,"height",member))==30);
 assert(!values.failed);wc_release(&compositor);
 puts("classic fields: measured row-pitch getter, intrinsic height, fixed leading and baseline positions passed");
}
