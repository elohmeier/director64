// Character positions must use the host's rendered rows and alignment,
// including live field edits and Windows1252 text returned by legacy FileIO.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static char measured[128];
static const lv_movie_t code={.name="CHARPOS.DXR"};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_text_style_t style={.font_name="Synthetic",.font_id=1,.size=12,.ascent=11,.line_height=14};
static const dg_member_t members[]={{.id=DG_ID(1,1,1),.number=1,.cast=1,.type=3,
 .width=40,.height=60,.name="Name",.text="abc",.text_style=&style}};
static const dg_frame_t frames[]={{0,0}};
static const dg_movie_t movie={.id=1,.code=&code,.tempo=30,.casts=casts,.cast_count=1,
 .members=members,.member_count=1,.frames=frames,.frame_count=1};
static int layout(void *ctx,const char *text,int width,int height,const wc_text_style_t *font,int16_t *boxes,int max) {
 (void)ctx;(void)height;assert(font&&font->font==1);
 snprintf(measured,sizeof(measured),"%s",text);
 int n=0,start=0,x=0,y=0;
 for(const unsigned char *p=(const unsigned char *)text;*p && n<max;n++) {
  unsigned cp=*p++;if(cp>=0xc0){while((*p&0xc0)==0x80)p++;}
  if((x+8>width&&x)||cp=='\r'||cp=='\n') {
   int offset=font->align==1?(width-x)/2:font->align==2?width-x:0;
   for(int i=start;i<n;i++)boxes[i*4]+=(int16_t)offset;
   x=0;y+=14;start=n;
  }
  if(cp=='\r'||cp=='\n') {memset(boxes+n*4,0,4*sizeof(*boxes));start=n+1;continue;}
  boxes[n*4]=(int16_t)x;boxes[n*4+1]=(int16_t)y;boxes[n*4+2]=8;boxes[n*4+3]=14;x+=8;
 }
 int offset=font->align==1?(width-x)/2:font->align==2?width-x:0;
 for(int i=start;i<n;i++)boxes[i*4]+=(int16_t)offset;
 return n;
}
static bool position(void *ctx,uint32_t member,int pos,int *x,int *y) {
 (void)ctx;const dg_member_t *m=dg_member(&director,member);const char *text=m->text;
 for(unsigned i=0;i<director.field_count;i++)if(director.field_members[i]==member && director.field_text_changed[i])
  text=lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT+i]);
 return wc_text_position(&compositor,&director,m,text,(int)m->width,pos,x,y);
}
static void at(lv_t member,int index,int x,int y) {
 lv_frame_t frame={.movie=&code};
 lv_t point=lv_call(&values,&frame,"charpostoloc",2,(lv_t[]){member,lv_num(index)});
 assert(!values.failed&&lv_integer(&values,lv_at(&values,point,1))==x&&lv_integer(&values,lv_at(&values,point,2))==y);
}
int main(void) {
 wc_init(&compositor,(wc_host_t){.layout=layout},1024);
 dg_init(&director,&values,(dg_platform_t){.text_position=position},NULL,NULL,0,1);
 assert(dg_enter(&director,&movie,1,NULL,0));lv_t member=lv_make(LV_MEMBER,members[0].id);
 at(member,1,0,13);at(member,2,8,13);at(member,4,24,13);
 lv_set(&values,NULL,"alignment",member,lv_symbol(&values,"center"));
 at(member,1,8,13);at(member,3,24,13);at(member,4,32,13);
 lv_set(&values,NULL,"alignment",member,lv_symbol(&values,"right"));at(member,1,16,13);
 lv_set(&values,NULL,"alignment",member,lv_symbol(&values,"center"));
 lv_set(&values,NULL,"text",member,lv_text(&values,"abcdefghijk",false));
 at(member,6,0,27);at(member,11,16,41);at(member,12,24,41);
 lv_set(&values,NULL,"text",member,lv_text(&values,"a\rb",false));at(member,3,16,27);
 lv_set(&values,NULL,"text",member,lv_text(&values,"\x80\x93x\x94",false));
 at(member,3,20,13);assert(!strcmp(measured,"\xe2\x82\xac\xe2\x80\x9cx\xe2\x80\x9d"));
 lv_set(&values,NULL,"text",member,lv_text(&values,"",false));at(member,1,20,13);
 wc_release(&compositor);
 puts("text positions: live alignment, wrapped rows, carets and Windows1252 glyphs passed");
}
