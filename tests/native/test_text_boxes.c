// Source-free boxed text geometry: structural observations from original
// Director8.5.1r104 under Wine, with deterministic authored test glyphs.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static unsigned measurements,rendered_face,rendered_font;
static const lv_movie_t code={.name="TEXTBOX.DXR"};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_text_style_t style={.font_name="Synthetic",.font_id=1,.size=12,.ascent=11,.line_height=14};
static const dg_member_t members[]={{.id=DG_ID(1,1,1),.number=1,.cast=1,.type=3,
  .width=40,.height=10,.name="Box",.text="abc",.text_style=&style}};
static const dg_frame_t frames[]={{0,0}};
static const dg_movie_t movie={.id=1,.code=&code,.tempo=30,.casts=casts,.cast_count=1,
  .members=members,.member_count=1,.frames=frames,.frame_count=1,.stage_color=0xffffff};
static int layout(void *ctx,const char *text,int width,int height,const wc_text_style_t *font,int16_t *boxes,int max) {
  (void)ctx;(void)height;assert(font&&(font->font==1||font->font==2));
  unsigned x=0,y=0;int n=0;
  for(;*text&&n<max;text++,n++) {
    if(*text=='\r'||*text=='\n') {x=0;y+=14;continue;}
    if(x+8>(unsigned)width&&x) {x=0;y+=14;}
    boxes[n*4]=(int16_t)x;boxes[n*4+1]=(int16_t)y;boxes[n*4+2]=8;boxes[n*4+3]=14;x+=8;
  }
  return n;
}
static bool text(void *ctx,const char *str,int width,int height,const wc_text_style_t *font,uint8_t *coverage) {
  rendered_face=font?font->face:0;rendered_font=font?font->font:0;
  int16_t boxes[128*4]={0};int n=layout(ctx,str,width,height,font,boxes,128);
  for(int i=0;i<n;i++)if(str[i]!='\r'&&str[i]!='\n'&&boxes[i*4]<width&&boxes[i*4+1]<height)
    coverage[boxes[i*4+1]*width+boxes[i*4]]=255;
  return true;
}
static int measure(void *ctx,uint32_t member,unsigned width) {
  (void)ctx;measurements++;
  const char *str=members[0].text;
  for(unsigned i=0;i<director.field_count;i++)if(director.field_members[i]==member)
    str=lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT+i]);
  return wc_text_height(&compositor,&director,&members[0],str,(int)width);
}
static bool image(void *ctx,uint32_t member,dg_pixel_image_t *out) {
  (void)ctx;return wc_read_image(&compositor,&director,member,out);
}
static int get(const char *name,lv_t member) {return lv_integer(&values,lv_get(&values,NULL,name,member));}
int main(void) {
  wc_init(&compositor,(wc_host_t){.text=text,.layout=layout},1024);
  dg_init(&director,&values,(dg_platform_t){.text_height=measure,.read_image=image},NULL,NULL,0,1);
  assert(dg_enter(&director,&movie,1,NULL,0));
  lv_t member=lv_make(LV_MEMBER,DG_ID(1,1,1));
  lv_set(&values,NULL,"boxtype",member,lv_symbol(&values,"fixed"));
  lv_set(&values,NULL,"width",member,lv_num(40));
  lv_set(&values,NULL,"height",member,lv_num(10));
  lv_set(&values,NULL,"text",member,lv_text(&values,"abcdefghijk",false));
  assert(get("height",member)==42&&get("width",member)==40&&get("pageheight",member)==10);
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"boxtype",member)),"fixed"));
  unsigned seen=measurements;assert(get("height",member)==42&&measurements==seen);
  lv_t snapshot=lv_get(&values,NULL,"image",member);
  assert(get("height",snapshot)==10&&get("width",snapshot)==40);
  director.sprites[1]=(dg_sprite_t){.value={.member=members[0].id,.type=16,.x=20,.y=30,
    .width=40,.height=10,.blend=100},.visible=true};
  int l,t,r,b;dg_bounds(&director,1,&l,&t,&r,&b);assert(l==20&&t==30&&r==60&&b==40);
  lv_set(&values,NULL,"boxtype",member,lv_symbol(&values,"adjust"));
  assert(get("height",member)==42&&get("pageheight",member)==42);
  snapshot=lv_get(&values,NULL,"image",member);assert(get("height",snapshot)==42);
  dg_bounds(&director,1,&l,&t,&r,&b);assert(b==72);
  lv_set(&values,NULL,"width",member,lv_num(80));assert(get("height",member)==28);
  lv_set(&values,NULL,"text",member,lv_text(&values,"abc\r",false));assert(get("height",member)==28);
  lv_set(&values,NULL,"boxtype",member,lv_symbol(&values,"scroll"));assert(get("pageheight",member)==10);
  lv_set(&values,NULL,"fontstyle",member,lv_list(&values,3,
    (lv_t[]){lv_symbol(&values,"bold"),lv_symbol(&values,"italic"),lv_symbol(&values,"underline")},false));
  (void)lv_get(&values,NULL,"image",member);assert(rendered_face==7);
  assert(lv_count(&values,lv_get(&values,NULL,"fontstyle",member))==3);
  lv_set(&values,NULL,"fontstyle",member,lv_list(&values,1,(lv_t[]){lv_symbol(&values,"plain")},false));
  (void)lv_get(&values,NULL,"image",member);assert(rendered_face==0);
  assert(!values.failed);
  lv_set(&values,NULL,"font",member,lv_text(&values,"Absent original face",false));
  (void)get("height",member);
  assert(values.failed&&strstr(values.error,"Absent original face"));
  dg_images_dispose(&director);wc_release(&compositor);
  // A recovered Arial fallback renders an absent requested face while .font
  // continues to report the request, as original8.5 does under Wine.
  const dg_text_style_t arial={.font_name="Arial",.font_id=2,.size=18,.ascent=17,.line_height=21};
  dg_member_t fallback_members[]={members[0],{.id=DG_ID(1,1,2),.number=2,.cast=1,.type=3,.text_style=&arial}};
  dg_movie_t fallback_movie=movie;fallback_movie.members=fallback_members;fallback_movie.member_count=2;
  wc_init(&compositor,(wc_host_t){.text=text,.layout=layout},1024);
  dg_init(&director,&values,(dg_platform_t){.text_height=measure,.read_image=image},NULL,NULL,0,1);
  assert(dg_enter(&director,&fallback_movie,1,NULL,0));
  lv_set(&values,NULL,"font",member,lv_text(&values,"Corsetta",false));
  (void)lv_get(&values,NULL,"image",member);
  assert(!values.failed&&rendered_font==2);
  assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"font",member)),"Corsetta"));
  dg_images_dispose(&director);wc_release(&compositor);
  puts("text boxes: intrinsic bounds, fixed/adjust/scroll pages, image crops and measurement invalidation passed");
}
