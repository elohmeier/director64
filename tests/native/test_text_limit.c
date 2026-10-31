#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static unsigned measurements;
static const lv_movie_t code={.name="LIMIT.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_member_t members[]={
 {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=3,.name="Limited",.text="",.editable=true,
  .width=40,.height=14,.text_box_present=true,.text_box_type=4,.text_word_wrap=true},
 {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=3,.name="Source fixed",.text="ABC",
  .width=145,.height=48,.text_box_present=true,.text_box_type=2,.text_word_wrap=true}};
static const dg_frame_t frame={0,0};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
 .members=members,.member_count=2,.frames=&frame,.frame_count=1};
static const char *text(uint32_t id) {
 for(unsigned i=0;i<director.field_count;i++)if(director.field_members[i]==id)
  return lv_cstr(&values,values.roots[DG_FIELD_TEXT_ROOT+i]);
 return members[(id&65535)-1].text;
}
static int measure(void *ctx,uint32_t id,unsigned width) {
 (void)ctx;measurements++;lv_collect(&values);
 if(id==members[1].id)return 15;
 assert(width==40);unsigned n=(unsigned)strlen(text(id));bool wrap=true;
 for(unsigned i=0;i<director.field_count;i++)if(director.field_members[i]==id)wrap=director.field_word_wrap[i];
 return (int)((wrap&&n?(n+4)/5:1)*14);
}
static bool position(void *ctx,uint32_t id,int index,int *x,int *y) {
 (void)ctx;assert(id==members[0].id&&index==(int)strlen(text(id))+1);
 *x=(index-1)*8;*y=13;return true;
}
static void edit(const char *value) {
 assert(dg_edit_text(&director,1,value));
 for(unsigned i=0;i<200&&director.text_pending;i++)assert(dg_tick(&director,0,0,false,1000));
 assert(!values.failed&&!director.text_pending);
}
int main(void) {
 dg_init(&director,&values,(dg_platform_t){.text_height=measure,.text_position=position},NULL,NULL,0,1);
 assert(dg_enter(&director,&movie,1,NULL,0));lv_t limited=lv_make(LV_MEMBER,members[0].id),fixed=lv_make(LV_MEMBER,members[1].id);
 // Untouched source geometry must work before a setter, without allocating
 // persistent mutable slots for every static text member encountered.
 assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"boxtype",fixed)),"fixed"));
 assert(!director.field_count&&!measurements);
 assert(lv_integer(&values,lv_get(&values,NULL,"pageheight",fixed))==48);
 assert(!director.field_count&&!measurements);
 assert(lv_integer(&values,lv_get(&values,NULL,"height",fixed))==15);
 assert(!director.field_count&&measurements==1);
 unsigned width=0,height=0;assert(dg_text_dimensions(&director,members[1].id,false,&width,&height));
 assert(width==145&&height==48&&!director.field_count);
 director.sprites[1]=(dg_sprite_t){.value={.member=members[0].id,.type=16,.width=40,.height=14},.visible=true};
 assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"boxtype",limited)),"limit"));
 edit("ABCDEF");assert(!strcmp(text(members[0].id),"ABCDE"));
 lv_set(&values,NULL,"text",limited,lv_text(&values,"ABCDEF",false));
 assert(!strcmp(text(members[0].id),"ABCDEF"));
 assert(lv_integer(&values,lv_get(&values,NULL,"height",limited))==28);
 assert(lv_integer(&values,lv_get(&values,NULL,"pageheight",limited))==14);
 lv_set(&values,NULL,"wordwrap",limited,lv_num(0));
 lv_set(&values,NULL,"text",limited,lv_text(&values,"",false));
 edit("ABCDEF");assert(!strcmp(text(members[0].id),"ABCDE"));
 lv_set(&values,NULL,"boxtype",limited,lv_symbol(&values,"fixed"));
 edit("ABCDEF");assert(!strcmp(text(members[0].id),"ABCDEF"));
 lv_set(&values,NULL,"boxtype",limited,lv_symbol(&values,"limit"));
 assert(!strcmp(lv_cstr(&values,lv_get(&values,NULL,"boxtype",limited)),"limit"));
 assert(!values.failed);puts("text limit: untouched source fixed viewport, keyboard-only wrap/caret limits and scripted overflow passed");
}
