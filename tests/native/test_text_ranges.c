// Formatting references preserve their cast identity while ordinary chunks
// remain strings. Glyph advances here are original8.5's Arial18 observations.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static uint32_t palette[256];
static const lv_movie_t code={.name="RANGES.DXR"};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_text_style_t style={.font_name="Arial",.font_id=1,.size=18,.ascent=17,.descent=4,.line_height=21};
static const dg_text_format_t initial[]={{.font=1,.size=18,.ascent=17,.line_height=21}};
static const dg_text_format_t tracked[]={
 {.font=1,.size=18,.ascent=17,.line_height=21,.char_spacing=-1000},
 {.offset=2,.font=1,.size=18,.ascent=17,.line_height=21,.char_spacing=1}};
static const dg_member_t members[]={{.id=DG_ID(1,1,1),.number=1,.cast=1,.type=3,.width=100,.height=42,
 .name="Field",.text="ABCD\rEF",.text_style=&style,.text_formats=initial,.text_format_count=1},
 {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=3,.width=100,.height=42,
 .name="Tracked",.text="ABCD",.text_style=&style,.text_formats=tracked,.text_format_count=2}};
static const dg_frame_t frames[]={{0,0}};
static const dg_movie_t movie={.id=1,.code=&code,.tempo=30,.casts=casts,.cast_count=1,
 .members=members,.member_count=2,.frames=frames,.frame_count=1,.palette=palette};
static int rich_layout(void *ctx,const char *text,int width,int height,const dg_text_format_t *formats,unsigned count,int16_t *boxes,int max) {
 (void)ctx;(void)width;(void)height;int x=0,y=0,n=0;unsigned run=0;
 for(;*text&&n<max;text++,n++) {
  while(run+1<count&&formats[run+1].offset<=(unsigned)n)run++;
  const dg_text_format_t *f=&formats[run];
  if(*text=='\r'||*text=='\n'){memset(boxes+n*4,0,4*sizeof(*boxes));x=0;y+=f->line_height;continue;}
  int advance=*text=='C'||*text=='D'?13:*text=='F'?11:12;
  boxes[n*4]=(int16_t)x;boxes[n*4+1]=(int16_t)y;boxes[n*4+2]=(int16_t)(advance+f->char_spacing);boxes[n*4+3]=f->line_height;
  x+=advance+f->char_spacing;
 }
 return n;
}
static int layout(void *ctx,const char *text,int width,int height,const wc_text_style_t *style_,int16_t *boxes,int max) {
 dg_text_format_t f={.size=(uint8_t)style_->size,.line_height=(int16_t)style_->line_height};
 return rich_layout(ctx,text,width,height,&f,1,boxes,max);
}
static bool position(void *ctx,uint32_t member,int index,int *x,int *y) {
 (void)ctx;const char *text=members[0].text;
 for(unsigned i=0;i<director.field_count;i++)if(director.field_members[i]==member)text=lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT+i]);
 return wc_text_position(&compositor,&director,&members[0],text,100,index,x,y);
}
static void at(int index,int x,int y) {int got_x,got_y;assert(position(NULL,members[0].id,index,&got_x,&got_y)&&got_x==x&&got_y==y&&!values.failed);}
static lv_t range(lv_t source,const char *kind,int first,int last) {
 lv_frame_t frame={.movie=&code};lv_t args[]={lv_text(&values,kind,false),lv_num(first),lv_num(last),source};
 lv_t proxy=lv_call(&values,&frame,"text_range",4,args);assert(lv_type(proxy)==LV_PROPLIST&&!values.failed);return proxy;
}
static const dg_text_format_t *format_at(unsigned offset) {
 unsigned count=0;const dg_text_format_t *formats=dg_text_formats(&director,members[0].id,&count);assert(formats&&count);
 unsigned i=0;while(i+1<count&&formats[i+1].offset<=offset)i++;return formats+i;
}
int main(void) {
 for(unsigned i=0;i<256;i++)palette[i]=0xffffff;
 palette[239]=0x112233;palette[5]=0x123456;palette[255]=0;
 wc_init(&compositor,(wc_host_t){.layout=layout,.rich_layout=rich_layout},1024);
 dg_init(&director,&values,(dg_platform_t){.text_position=position},NULL,NULL,0,1);assert(dg_enter(&director,&movie,1,NULL,0));
 lv_t ordinary=lv_list(&values,6,(lv_t[]){lv_num(1),lv_num(9),lv_symbol(&values,"kind"),lv_num(4),
                                        lv_symbol(&values,"color"),lv_num(2)},true);
 assert(lv_integer(&values,lv_get(&values,NULL,"color",ordinary))==2&&!values.failed);
 lv_set(&values,NULL,"color",ordinary,lv_num(3));
 assert(lv_integer(&values,lv_get(&values,NULL,"color",ordinary))==3&&!values.failed);
 lv_t member=lv_make(LV_MEMBER,members[0].id);at(1,0,20);at(3,24,20);at(6,0,41);
 // Original 8.5 returns RGB color objects for color and palette integers
 // for foreColor, including mutable and nested character ranges.
 assert(lv_type(lv_get(&values,NULL,"color",member))==LV_COLOR);
 assert(lv_integer(&values,lv_get(&values,NULL,"forecolor",member))==255);
 lv_set(&values,NULL,"color",member,lv_make(LV_COLOR,0x112233));
 assert(lv_integer(&values,lv_get(&values,NULL,"forecolor",member))==239);
 lv_t color_second=range(member,"char",2,2);
 assert(lv_type(lv_get(&values,NULL,"color",color_second))==LV_COLOR);
 assert(lv_integer(&values,lv_get(&values,NULL,"forecolor",color_second))==239);
 lv_set(&values,NULL,"forecolor",color_second,lv_num(255));
 assert(lv_integer(&values,lv_get(&values,NULL,"color",color_second))==0);
 assert(lv_integer(&values,lv_get(&values,NULL,"forecolor",color_second))==255);
 lv_t color_first=range(range(member,"word",1,1),"char",1,1);
 assert(lv_integer(&values,lv_get(&values,NULL,"forecolor",color_first))==239);
 lv_set(&values,NULL,"color",member,lv_make(LV_COLOR,0));
 lv_set(&values,NULL,"alignment",member,lv_symbol(&values,"full"));
 assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"alignment",member)),"full"));
 assert(format_at(0)->align==3);
 lv_set(&values,NULL,"alignment",member,lv_symbol(&values,"left"));
 lv_set(&values,NULL,"fixedlinespace",member,lv_num(32));at(1,0,31);at(6,0,63);
 lv_set(&values,NULL,"charspacing",member,lv_num(2));at(2,14,31);at(3,28,31);at(8,27,63);
 lv_t b=range(member,"char",2,2);lv_set(&values,NULL,"charspacing",b,lv_num(4));at(2,14,31);at(3,30,31);
 assert(lv_integer(&values,lv_get(&values,NULL,"charspacing",b))==4);
 lv_set(&values,NULL,"charspacing",member,lv_num(0));lv_set(&values,NULL,"fixedlinespace",member,lv_num(10));at(1,0,9);at(6,0,19);
 lv_set(&values,NULL,"fixedlinespace",member,lv_num(0));at(1,0,20);at(6,0,41);
 lv_t ab=range(member,"char",1,2);lv_set(&values,NULL,"fontsize",ab,lv_num(12));
 assert(format_at(0)->size==12&&format_at(1)->size==12&&format_at(2)->size==18);
 lv_t bc=range(member,"char",2,3);lv_set(&values,NULL,"color",bc,lv_make(LV_COLOR,0x123456));
 assert(format_at(0)->rgb==0&&format_at(1)->rgb==0x123456&&format_at(2)->rgb==0x123456&&format_at(3)->rgb==0);
 lv_set(&values,NULL,"fontstyle",b,lv_list(&values,2,(lv_t[]){lv_symbol(&values,"bold"),lv_symbol(&values,"italic")},false));
 assert(format_at(1)->face==3&&format_at(0)->face==0&&lv_count(&values,lv_get(&values,NULL,"fontstyle",b))==2);
 lv_t word=range(member,"word",1,1),nested=range(word,"char",2,2);lv_set(&values,NULL,"charspacing",nested,lv_num(9));assert(format_at(1)->char_spacing==9&&format_at(0)->char_spacing==0);
 lv_t chunk=lv_chunk(&values,"char",member,lv_num(2),lv_num(3));assert(lv_type(chunk)==LV_STRING&&!strcmp(lv_cstr(&values,chunk),"BC"));
 lv_set(&values,NULL,"text",member,lv_text(&values,"\xc3\xa4" "bc",false));
 lv_t unicode=range(member,"char",2,2);lv_set(&values,NULL,"fontsize",unicode,lv_num(24));
 assert(format_at(0)->size==18&&format_at(1)->size==24&&format_at(2)->size==18);
 // Preserve old styles around insertion/deletion and inherit the styled
 // character for put-after text, matching the member reference VM path.
 lv_set(&values,NULL,"text",member,lv_text(&values,"ABCD",false));
 b=range(member,"char",2,2);lv_set(&values,NULL,"textsize",b,lv_num(24));
 lv_set(&values,NULL,"textcolor",b,lv_make(LV_COLOR,0x123456));
 lv_set(&values,NULL,"charspacing",b,lv_num(4));
 assert(dg_text_format_splice(&director,members[0].id,"char",lv_num(2),lv_num(2),lv_text(&values,"Bi",false),lv_text(&values,"ABiCD",false)));
 assert(format_at(1)->size==24&&format_at(2)->size==24&&format_at(2)->rgb==0x123456&&format_at(3)->size==18&&format_at(3)->rgb==0);
 assert(dg_text_format_splice(&director,members[0].id,"char",lv_num(2),lv_num(2),(lv_t){0},lv_text(&values,"AiCD",false)));
 assert(format_at(1)->size==24&&format_at(1)->char_spacing==4&&format_at(2)->size==18);
 assert(dg_text_format_splice(&director,members[0].id,"char",lv_num(1),lv_num(1),lv_text(&values,"ZA",false),lv_text(&values,"ZAiCD",false)));
 assert(format_at(0)->size==18&&format_at(1)->size==18&&format_at(2)->size==24&&format_at(3)->size==18);
 lv_set(&values,NULL,"text",member,lv_text(&values,"AA BB CC",false));
 lv_t middle=range(member,"word",2,2);lv_set(&values,NULL,"color",middle,lv_make(LV_COLOR,0xabcdef));
 assert(dg_text_format_splice(&director,members[0].id,"word",lv_num(2),lv_num(2),(lv_t){0},lv_text(&values,"AA CC",false)));
 assert(format_at(2)->rgb==0&&format_at(3)->rgb==0);
 lv_set(&values,NULL,"textsize",member,lv_num(0));
 assert(lv_integer(&values,lv_get(&values,NULL,"fontsize",member))==0&&format_at(0)->size==0&&format_at(0)->line_height==14);
 lv_t zero=range(member,"char",2,2);assert(lv_integer(&values,lv_get(&values,NULL,"textsize",zero))==0);
 lv_set(&values,NULL,"textsize",member,lv_num(12));assert(format_at(0)->size==12&&format_at(0)->line_height==14);
 // Replacing text inherits first-character tracking, not the last run or 0.
 lv_set(&values,NULL,"charspacing",range(member,"char",1,2),lv_num(3));
 lv_set(&values,NULL,"charspacing",range(member,"char",3,4),lv_num(-2));
 lv_set(&values,NULL,"text",member,lv_text(&values,"XY",false));
 assert(lv_integer(&values,lv_get(&values,NULL,"charspacing",member))==3&&format_at(1)->char_spacing==3);
 lv_t source_tracked=lv_make(LV_MEMBER,members[1].id);
 lv_set(&values,NULL,"text",source_tracked,lv_text(&values,"replacement",false));
 assert(lv_integer(&values,lv_get(&values,NULL,"charspacing",source_tracked))==-1000);
 unsigned tracked_count=0;const dg_text_format_t *preserved=dg_text_formats(&director,members[1].id,&tracked_count);
 assert(tracked_count==1&&preserved[0].offset==0&&preserved[0].char_spacing==-1000);
 lv_set(&values,NULL,"textsize",member,lv_num(18));
 lv_set(&values,NULL,"text",member,lv_text(&values,"ABCD",false));
 lv_set(&values,NULL,"charspacing",member,lv_num(-1000));
 at(1,0,20);at(2,-988,20);at(3,-1976,20);at(5,-3950,20);
 assert(wc_text_char_at(&compositor,&director,&members[0],"ABCD",100,42,-500,5)==0);
 assert(!values.failed);wc_release(&compositor);
 puts("text ranges: member identity, fixed leading, additive tracking, overlapping styles, nested ranges and Unicode offsets passed");
}
