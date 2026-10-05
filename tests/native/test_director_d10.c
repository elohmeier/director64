#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
// The D10 profile shares D7's verified score semantics; its bounds come from
// the recovered Lernerfolg corpus (sprite channels through 983 in 1,006-channel
// scores, a window movie plus eleven linked external cast files).
static dg_runtime_t d;
static lv_runtime_t r;
static unsigned clicked, global_calls, sounds, empty_sounds;
static char saved[128];
static unsigned hit_calls;
static bool hit(void *ctx,const dg_member_t *m,unsigned ink,int x,int y) {
 (void)ctx;(void)m;(void)ink;(void)x;(void)y;hit_calls++;return true;
}
static const char *date(void *ctx) {(void)ctx;return "Sunday, December 24, 2000";}
static char ini[256];
static bool read_file(void *ctx,const char *name,char *out,unsigned cap,unsigned *length) {
 (void)ctx;
 if(!strcmp(name,"start.ini")) {
  if(!*ini)return false;
  assert(strlen(ini)<cap);strcpy(out,ini);*length=strlen(ini);return true;
 }
 if(strcmp(name,"Lernwort.txt"))return false;
 if(!*saved)return false;
 assert(strlen(saved)<cap);strcpy(out,saved);*length=strlen(saved);return true;
}
static bool write_file(void *ctx,const char *name,const char *data,unsigned length) {
 (void)ctx;
 if(!strcmp(name,"start.ini")) {
  assert(length<sizeof(ini));memcpy(ini,data,length);ini[length]=0;return true;
 }
 assert(!strcmp(name,"Lernwort.txt") && length<sizeof(saved));
 memcpy(saved,data,length);saved[length]=0;return true;
}
static void sound(void *ctx,unsigned channel,const dg_member_t *m) {(void)ctx;(void)channel;if(m)sounds++;}
static void trace(void *ctx,const char *s) {(void)ctx;if(strstr(s,"SCORE_SOUND_EMPTY"))empty_sounds++;}
static lv_flow_t local(lv_runtime_t *v,lv_frame_t *f) {
 assert(lv_type(f->self)==LV_VOID && lv_numeric(f->locals[0])==42);
 if(!f->pc++)return LV_YIELD;
 v->result=lv_num(99);clicked++;return LV_RETURN;
}
static lv_flow_t up(lv_runtime_t *v,lv_frame_t *f) {
 if(!f->pc++)return lv_invoke(v,f,"helper",1,(lv_t[]){lv_num(42)});
 assert(lv_numeric(v->result)==99);return LV_RETURN;
}
static lv_flow_t global(lv_runtime_t *v,lv_frame_t *f) {(void)v;(void)f;global_calls++;return LV_RETURN;}
static const lv_handler_t handlers[]={
 {.name="mouseup",.member=1,.cast="Internal",.kind="BehaviorScript",.step=up},
 {.name="helper",.member=1,.cast="Internal",.kind="BehaviorScript",.arguments=1,.locals=1,.step=local},
 {.name="helper",.member=2,.cast="Internal",.kind="MovieScript",.step=global},
 {.name="mouseup",.member=2,.cast="Internal",.kind="MovieScript",.step=global},
};
static const lv_movie_t code={"D10.DXR",4,handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[]={{"Internal",1,1},{"Extra",1,2}};
// The recorded dynamic surface of a converted Flash card: feedback-state
// frame labels and one named edit-text field with measured advances.
static const dg_label_t card_labels[]={{"normal",1},{"red",2},{"green",3}};
static const uint8_t card_advances[224]={
 [0]=6,[1]=6,['H'-32]=8,['a'-32]=6,['u'-32]=6,['s'-32]=5,['e'-32]=6,
 ['r'-32]=4,[0xE4-32]=6};
static const dg_text_style_t card_style={.font_name="TestFont",.font_id=1,
 .size=12,.align=1,.ascent=10,.descent=2,.leading=0,.line_height=12,
 .color=0x102030,.advances=card_advances,.advance_count=224};
static const dg_flash_field_t card_fields[]={
 {.name="my_txt",.variable="text1",.text="",.x=4,.y=6,.width=104,.height=40,
  .align=1,.word_wrap=1,.multiline=1,.style=&card_style}};
static const dg_member_t members[]={
 {.id=0x12000a,.number=10,.cast=2,.type=1,.width=16,.height=16,.name="first"},
 // A converted Flash member answers its authored type to scripts.
 {.id=0x12000b,.number=11,.cast=2,.type=1,.width=16,.height=16,.name="second",.source_xtra=1,
  .flash_label_count=3,.flash_labels=card_labels,
  .flash_field_count=1,.flash_fields=card_fields},
 // An editable name field for the on-screen keyboard contract.
 {.id=0x12000c,.number=12,.cast=2,.type=3,.name="entry",.text="",.editable=true},
 // A two-second speech member: room ambience sizes its waits from endTime.
 {.id=0x12000d,.number=13,.cast=2,.type=6,.name="speech",.samples=44100,.rate=22050},
};
static const dg_delta_t deltas[]={
 {.channel=0,.mask=DG_ALL,.value={.script=0x110001}},
 {.channel=3,.mask=DG_ALL,.value={.member=0x11007c}},
 // The last authorable channel: sprite 1000, beyond every earlier profile.
 {.channel=1005,.mask=DG_ALL,.value={.member=0x12000a,.type=1}},
};
static const dg_frame_t frames[]={{0,3}};
static const dg_movie_t movie={.code=&code,.id=1,.tempo=1,.cast_count=2,.casts=casts,.members=members,.member_count=4,
 .frame_count=1,.frames=frames,.deltas=deltas};
static lv_flow_t iface(lv_runtime_t *v,lv_frame_t *f) {(void)f;v->result=lv_num(77);return LV_RETURN;}
static const lv_handler_t control_handlers[]={
 {.name="geticommon",.member=1,.cast="Internal",.kind="MovieScript",.step=iface},
};
static const lv_movie_t control_code={"CONTROL.DXR",1,control_handlers,NULL, NULL, NULL, NULL, 0};
static const dg_frame_t empty_frames[]={{0,0}};
static const dg_delta_t empty_deltas[]={{0,0,{0}}};
static const dg_movie_t control={.code=&control_code,.id=2,.tempo=1,
 .frame_count=1,.frames=empty_frames,.deltas=empty_deltas};
static const lv_movie_t check_code={"CHECK.DXR",0,NULL,NULL, NULL, NULL, NULL, 0};
static const dg_movie_t check={.code=&check_code,.id=3,.tempo=1,
 .frame_count=1,.frames=empty_frames,.deltas=empty_deltas};
// An alternate cast archive: castLib fileName swaps rebind the "Extra"
// library onto this file's External cast.
static const dg_member_t extra2_members[]={
 {.id=0x41000a,.number=10,.cast=1,.type=1,.width=8,.height=8,.name="alt"},
};
static const dg_cast_t extra2_casts[]={{"External",4,1}};
static const lv_movie_t extra2_code={"EXTRA2.CXT",0,NULL,NULL, NULL, NULL, NULL, 0};
static const dg_movie_t extra2={.code=&extra2_code,.id=4,.tempo=1,.cast_count=1,
 .casts=extra2_casts,.members=extra2_members,.member_count=1,
 .frame_count=1,.frames=empty_frames,.deltas=empty_deltas};
static const dg_movie_t *find_movie(void *ctx,const char *stem,unsigned file) {
 (void)ctx;
 if(stem?!strcmp(stem,"extra2"):file==4)return &extra2;
 return NULL;
}
// One shipped read-only data file: the exercise task databases resolve by
// their authored folder tail, with any Windows path above it discarded.
static char requested_data[64];
static bool read_data(void *ctx,const char *name,char *out,unsigned cap,unsigned *length) {
 (void)ctx;
 snprintf(requested_data,sizeof(requested_data),"%s",name);
 if(strcmp(name,"tests_db/Test1A/1-01-A.test.db"))return false;
 static const unsigned char blob[]={104,0,255,7};
 assert(cap>=sizeof(blob));memcpy(out,blob,sizeof(blob));*length=sizeof(blob);return true;
}
static const char *const global_names[]={"gone","gtwo"};
static lv_t call(const char *name,unsigned n,const lv_t *a) {
 lv_frame_t f={.movie=&code,.handler=&handlers[0]};return lv_call(&r,&f,name,n,a);
}
int main(void) {
 assert(DG_MAX_TEMPO==999 && DG_SPRITES==1001 && DG_SCORE_CHANNELS==1006);
 assert(DG_FILES==25 && LV_SHARED==24);
 dg_init(&d,&r,(dg_platform_t){.long_date=date,.read_file=read_file,.write_file=write_file,
  .sound=sound,.trace=trace,.hit=hit,.find_movie=find_movie,.read_data=read_data},
  NULL,global_names,2,42);
 assert(dg_enter(&d,&movie,1,NULL,0));
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"longdate",(lv_t){0})),date(NULL)));
 assert(dg_tick(&d,320,240,true,1000));
 for(unsigned i=0;i<4;i++)assert(dg_tick(&d,320,240,false,1000));
 assert(clicked==1 && !global_calls && !sounds && empty_sounds==1);
 // The channel-0 frame-behavior span delivered its beginSprite.
 assert(d.frame_script_member==0x110001 && !d.frame_script_begin_pending);
 assert(d.sprites[1000].value.member==0x12000a);
 for(unsigned i=0;i<2;i++) {
  lv_t file=call("fileio",3,(lv_t[]){lv_text(&r,"mnew",true),lv_text(&r,"append",false),lv_text(&r,"Lernwort.txt",false)});
  assert(lv_type(file)==LV_FILE);
  call("file_method",3,(lv_t[]){file,lv_text(&r,"mwritestring",true),lv_text(&r,i?"two\r":"one\r",false)});
  call("file_method",2,(lv_t[]){file,lv_text(&r,"mdispose",true)});
 }
 assert(!r.failed && !strcmp(saved,"one\rtwo\r"));
 // The on-screen keyboard commits through dg_edit_text: windows-1252
 // German letters type into names and spelling answers; other non-ASCII
 // and quote characters stay rejected.
 lv_set(&r,NULL,"member",lv_make(LV_SPRITE, 5),lv_make(LV_MEMBER, 0x12000c));
 assert(!dg_edit_text(&d,5,"B\"R"));
 assert(!dg_edit_text(&d,5,"B\x9FR"));
 assert(dg_edit_text(&d,5,"B\xC4R"));
 assert(d.text_pending && !strcmp(d.text_replacement,"B\xC4R"));
 d.text_pending=false;
 call("puppettransition",1,(lv_t[]){lv_num(9)});
 assert(!r.failed && d.transition_type==9 && d.transition_duration==15);
 lv_t sprite=lv_make(LV_SPRITE, 1);
 lv_set(&r,NULL,"membernum",sprite,lv_num(10));
 assert(!r.failed && d.sprites[1].value.member==0x11000a);
 lv_set(&r,NULL,"member",sprite,lv_make(LV_MEMBER, 0x12000a));
 assert(lv_numeric(lv_get(&r,NULL,"membernum",sprite))==10);
 assert(lv_numeric(lv_get(&r,NULL,"castnum",sprite))==131082);
 lv_set(&r,NULL,"membernum",sprite,lv_num(11));
 assert(d.sprites[1].value.member==0x12000b);
 lv_set(&r,NULL,"castnum",sprite,lv_num(131082));
 assert(d.sprites[1].value.member==0x12000a);
 lv_set(&r,NULL,"puppet",sprite,lv_num(1));
 assert(lv_numeric(lv_get(&r,NULL,"puppet",sprite))==1);
 call("cursor",1,(lv_t[]){lv_list(&r,2,(lv_t[]){lv_num(2000),lv_num(2001)},false)});
 assert(!r.failed && d.cursor.image==0);
 lv_set(&r,NULL,"moveablesprite",sprite,lv_num(1));
 lv_set(&r,NULL,"visible",sprite,lv_num(1));
 lv_t top=lv_make(LV_SPRITE, 1000);
 lv_set(&r,NULL,"moveablesprite",top,lv_num(1));
 lv_set(&r,NULL,"visible",top,lv_num(1));
 hit_calls=0;
 // The hover cache serves repeated polls, and the top authored channel wins.
 for(unsigned i=0;i<100;i++)assert(dg_mouse_hit(&d,5,5)==1000);
 assert(hit_calls==1);
 lv_set(&r,NULL,"visible",top,lv_num(0));
 assert(dg_mouse_hit(&d,5,5)==1);
 lv_set(&r,NULL,"visible",top,lv_num(1));
 assert(dg_mouse_hit(&d,5,5)==1000 && hit_calls==3);
 // Hover transitions sample once per tick: after a handler invalidates the
 // cached hit, further service passes in the same tick do not rescan the
 // score; the next tick samples again.
 assert(dg_tick(&d,5,5,false,1000) && hit_calls==4);
 d.mouse_hit_valid=false;
 assert(dg_service(&d,1000) && hit_calls==4);
 assert(dg_tick(&d,5,5,false,1000) && hit_calls==5);
 // A sprite answers the cast library of the member it shows.
 assert(lv_numeric(lv_get(&r,NULL,"castlibnum",top))==2);
 // baReadBinFile keeps the authored folder tail and answers the file's
 // bytes as a list; an absent file answers VOID, as the Xtra did.
 lv_t blob=call("bareadbinfile",1,(lv_t[]){
  lv_text(&r,"C:\\Programme\\Deutsch\\tests_db\\Test1A\\1-01-A.test.db",false)});
 assert(!strcmp(requested_data,"tests_db/Test1A/1-01-A.test.db"));
 // Byte storage: one byte per element, not a value each, and it answers the
 // list protocol the authored parser uses.
 assert(lv_type(blob)==LV_BYTES && lv_count(&r,blob)==4);
 assert(lv_numeric(lv_at(&r,blob,1))==104 && lv_numeric(lv_at(&r,blob,3))==255);
 assert(lv_numeric(lv_get(&r,NULL,"count",blob))==4);
 assert(lv_numeric(call("listp",1,(lv_t[]){blob}))==1);
 assert(!strcmp(lv_cstr(&r,call("ilk",1,(lv_t[]){blob})),"list"));
 assert(lv_numeric(lv_index_get(&r,blob,lv_num(2)))==0);
 assert(lv_type(call("bareadbinfile",1,(lv_t[]){lv_text(&r,"tests_db\\None\\x.db",false)}))==LV_VOID);
 // Flash sprites expose the named objects inside their timeline. A name
 // matching a recorded edit field seeds from its descriptor; writes read
 // back, setText decodes the authored percent-escapes and lays the text
 // out, and the sprite answers its own timeline root.
 lv_set(&r,NULL,"member",top,lv_make(LV_MEMBER, 0x12000b));
 lv_t root=lv_get(&r,NULL,"_root",top);
 assert(lv_type(root)==LV_SPRITE && lv_id(root)==lv_id(top));
 lv_t field=lv_get(&r,NULL,"my_txt",top);
 assert(!r.failed && lv_type(field)==LV_PROPLIST);
 assert(lv_numeric(lv_get(&r,NULL,"_y",field))==6);
 assert(lv_numeric(lv_get(&r,NULL,"_width",field))==104);
 assert(lv_numeric(lv_get(&r,NULL,"size",field))==12);
 lv_set(&r,NULL,"text",field,lv_text(&r,"Haus",false));
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",lv_get(&r,NULL,"my_txt",top))),"Haus"));
 // setText: "H%c3%a4user" decodes to UTF-8 Häuser; one 12-pixel line.
 call("settext",2,(lv_t[]){field,lv_text(&r,"H%c3%a4user",false)});
 assert(!r.failed);
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",field)),"H\xC3\xA4user"));
 assert(lv_numeric(lv_get(&r,NULL,"textheight",field))==12);
 // Wrapping: the field's 96-pixel inner width breaks at the space.
 call("settext",2,(lv_t[]){field,
  lv_text(&r,"Haus Haus Haus Haus Haus Haus",false)});
 assert(lv_numeric(lv_get(&r,NULL,"textheight",field))==24);
 // getTextFormat/setTextFormat carry the whole-field size and color; the
 // laid-out height follows the new size.
 call("settext",2,(lv_t[]){field,lv_text(&r,"Haus",false)});
 lv_t format=call("gettextformat",1,(lv_t[]){field});
 assert(lv_type(format)==LV_PROPLIST && lv_numeric(lv_get(&r,NULL,"size",format))==12);
 lv_set(&r,NULL,"size",format,lv_num(24));
 call("settextformat",2,(lv_t[]){field,format});
 assert(lv_numeric(lv_get(&r,NULL,"size",field))==24);
 assert(lv_numeric(lv_get(&r,NULL,"textheight",field))==24);
 // setVariable reaches the field through its bound variable name.
 call("setvariable",3,(lv_t[]){top,lv_text(&r,"text1",false),lv_text(&r,"neu",false)});
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",lv_get(&r,NULL,"my_txt",top))),"neu"));
 assert(!strcmp(lv_cstr(&r,call("getvariable",2,(lv_t[]){top,lv_text(&r,"text1",false)})),"neu"));
 // setFlashProperty visibility reads back; a field's toggle gates its overlay.
 call("setflashproperty",4,(lv_t[]){top,lv_text(&r,"my_txt",false),
  lv_text(&r,"visible",true),lv_num(0)});
 assert(lv_numeric(call("getflashproperty",3,(lv_t[]){top,lv_text(&r,"my_txt",false),
  lv_text(&r,"visible",true)}))==0);
 // Frame labels resolve on the flattened timeline; findLabel answers zero
 // for an unknown label so the authored fallback seeks frame one.
 assert(lv_numeric(call("findlabel",2,(lv_t[]){top,lv_text(&r,"red",false)}))==2);
 assert(lv_numeric(call("findlabel",2,(lv_t[]){top,lv_text(&r,"blau",false)}))==0);
 call("sprite_go",2,(lv_t[]){top,lv_text(&r,"green",false)});
 assert(d.sprites[1000].film_frame==2);
 call("sprite_go",2,(lv_t[]){top,lv_text(&r,"blau",false)});
 assert(d.sprites[1000].film_frame==2);
 // The recovered JavaScript-dialect handlers' native semantics.
 assert(!strcmp(lv_cstr(&r,call("js_int2hex",1,(lv_t[]){lv_num(195)})),"c3"));
 assert(!strcmp(lv_cstr(&r,call("js_int2hex",1,(lv_t[]){lv_num(10)})),"a"));
 call("js_cleargarbage",0,NULL);
 assert(!r.failed);
 lv_set(&r,NULL,"member",top,lv_make(LV_MEMBER, 0x12000a));
 // Flash playback settings on a converted member are accepted and ignored.
 lv_set(&r,NULL,"static",lv_make(LV_MEMBER, 0x12000b),lv_num(1));
 assert(!r.failed);
 // A playing channel answers the member's length in milliseconds.
 call("puppetsound",2,(lv_t[]){lv_num(1),lv_make(LV_MEMBER, 0x12000d)});
 assert(!r.failed);
 assert(lv_numeric(lv_get(&r,NULL,"endtime",lv_make(LV_SOUND, 1)))==2000);
 call("unload",2,(lv_t[]){lv_num(1),lv_num(9)});
 lv_frame_t caller={.movie=&code,.handler=&handlers[0]};
 assert(lv_invoke(&r,&caller,"go",2,(lv_t[]){lv_num(1),lv_text(&r,"check.dir",false)})==LV_YIELD);
 assert(!r.failed && !strcmp(d.next_movie,"check.DXR"));
 // Controller window: fileName attach keeps the window movie's code
 // resident, its handlers resolve from stage code, navigation inside a
 // tell retargets the window, and stage navigation appends .DXR to stems.
 lv_t win=call("window",1,(lv_t[]){lv_text(&r,"control",false)});
 assert(!r.failed && lv_type(win)==LV_WINDOW);
 lv_set(&r,NULL,"filename",win,lv_text(&r,"@:global:control.dir",false));
 assert(!r.failed && !strcmp(d.window_movie_name,"control") && !d.window_movie);
 assert(lv_numeric(call("windowpresent",1,(lv_t[]){lv_text(&r,"Control",false)}))==1);
 // A navigation issued while the load is pending queues in authored order.
 call("tell_window",1,(lv_t[]){win});
 assert(d.tell_window);
 call("gotomovie",1,(lv_t[]){lv_text(&r,"check",false)});
 call("tell_end",0,NULL);
 assert(!r.failed && !strcmp(d.window_movie_name,"control") && !strcmp(d.window_movie_queue,"check"));
 dg_window_attach(&d,&control);
 assert(d.window_movie==&control && d.window_home==&control);
 assert(lv_numeric(call("geticommon",0,NULL))==77 && !r.failed);
 dg_window_loaded(&d);
 assert(!strcmp(d.window_movie_name,"check") && !d.window_movie && !d.window_movie_queue[0]);
 dg_window_attach(&d,&check);
 assert(d.window_movie==&check && d.window_home==&control);
 // The home controller's handlers stay resident beside the focused movie.
 assert(lv_numeric(call("geticommon",0,NULL))==77 && !r.failed);
 assert(lv_numeric(call("baregister",2,(lv_t[]){lv_text(&r,"vendor",false),lv_num(1)}))==1);
 assert(lv_invoke(&r,&caller,"gotomovie",1,(lv_t[]){lv_text(&r,"mainscr",false)})==LV_YIELD);
 assert(!r.failed && !strcmp(d.next_movie,"mainscr.DXR"));
 lv_set(&r,NULL,"visible",win,lv_num(1));
 assert(d.window_visible);
 lv_set(&r,NULL,"rect",win,lv_rect(&r,4,53,174,578));
 lv_t bounds=lv_get(&r,NULL,"drawrect",win);
 assert(!r.failed && lv_integer(&r,lv_at(&r,bounds,1))==4 && lv_integer(&r,lv_at(&r,bounds,4))==578);
 lv_t gone=lv_get(&r,NULL,"forget",win);
 (void)gone;
 assert(!d.window_open && !d.window_visible);
 // Environment answers, existence probes and input drains for authored
 // startup checks.
 assert(lv_numeric(lv_get(&r,NULL,"lastchannel",(lv_t){0}))==1000);
 assert(lv_numeric(call("marker",1,(lv_t[]){lv_text(&r,"absent",false)}))==0);
 assert(lv_type(lv_get(&r,NULL,"rollover",(lv_t){0}))==LV_NUMBER);
 lv_t volumes=call("fx_volumestolist",1,(lv_t[]){(lv_t){0}});
 assert(lv_type(volumes)==LV_LIST && lv_count(&r,volumes)==0);
 assert(lv_numeric(call("fx_fileexists",2,(lv_t[]){(lv_t){0},lv_text(&r,"C:\\data\\login.dxr",false)}))==1);
 assert(!strcmp(lv_cstr(&r,call("bamultidisplayinfo",2,
  (lv_t[]){lv_text(&r,"primary",false),lv_text(&r,"width",false)})),"800"));
 call("flushinputevents",0,NULL);
 assert(!r.failed);
 assert(lv_numeric(call("isbusy",1,(lv_t[]){lv_make(LV_SOUND, 1)}))==0);
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"type",lv_make(LV_MEMBER, 0x12000b))),"flash"));
 // Buddy API INI persistence: default when absent, roundtrip after writes,
 // and section preservation across a second key.
 lv_t ini_args[]={lv_text(&r,"Debug",false),lv_text(&r,"CheckCD",false),
  lv_text(&r,"1",false),lv_text(&r,"C:\\start.ini",false)};
 assert(!strcmp(lv_cstr(&r,call("bareadini",4,ini_args)),"1"));
 call("bawriteini",4,(lv_t[]){ini_args[0],ini_args[1],lv_text(&r,"0",false),ini_args[3]});
 call("bawriteini",4,(lv_t[]){lv_text(&r,"Settings",false),lv_text(&r,"Width",false),
  lv_text(&r,"800",false),ini_args[3]});
 assert(!strcmp(lv_cstr(&r,call("bareadini",4,ini_args)),"0"));
 assert(!strcmp(lv_cstr(&r,call("bareadini",4,(lv_t[]){lv_text(&r,"Settings",false),
  lv_text(&r,"Width",false),lv_text(&r,"9",false),ini_args[3]})),"800"));
 // The globals view reads and writes the named global slots directly.
 lv_t view=lv_get(&r,NULL,"globals",(lv_t){0});
 assert(lv_type(view)==LV_GLOBALS_VIEW && lv_count(&r,view)==2);
 lv_index_set(&r,view,lv_text(&r,"gtwo",true),lv_num(5));
 assert(lv_numeric(r.globals[1])==5);
 assert(lv_numeric(lv_index_get(&r,view,lv_text(&r,"gtwo",true)))==5);
 lv_t first_name=call("getpropat",2,(lv_t[]){view,lv_num(1)});
 assert(lv_type(first_name)==LV_SYMBOL && !strcmp(lv_cstr(&r,first_name),"gone"));
 // call(#handler, receiver) skips a receiver without the handler.
 lv_t inst=lv_instance(&r,lv_make(LV_SCRIPT, 0x110001));
 assert(lv_invoke(&r,&caller,"call",2,(lv_t[]){lv_text(&r,"nosuch",true),inst})==LV_CONTINUE);
 assert(!r.failed);
 // castLib fileName rebinds the named library onto another cast archive:
 // member lookups resolve into its External cast and the getter answers
 // the swapped stem.
 lv_t lib=lv_reference(&r,"castlib",lv_text(&r,"Extra",false),(lv_t){0});
 assert(lv_type(lib)==LV_CASTLIB && lv_id(lib)==2);
 lv_set(&r,NULL,"filename",lib,lv_text(&r,"C:\\data\\extra2.cst",false));
 assert(!r.failed && d.cast_swap_count==1 && d.cast_swaps[0].target==&extra2);
 lv_t swapped=lv_reference(&r,"member",lv_num(10),lv_text(&r,"Extra",false));
 assert(lv_type(swapped)==LV_MEMBER && (uint32_t)lv_id(swapped)==0x41000au);
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"name",swapped)),"alt"));
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"filename",lib)),"extra2"));
 // Emergency in-place collection: when one step outgrows the free
 // remainder, garbage from earlier steps frees without moving anything —
 // rooted values keep their content AND their raw heap addresses, and
 // values born during the current step survive unrooted.
 {
  static char junk[8192];
  memset(junk,'x',sizeof(junk)-1);junk[sizeof(junk)-1]=0;
  lv_collect(&r);
  lv_t keep=lv_text(&r,"keepsake",false);
  r.globals[0]=keep;
  const char *keep_raw=lv_cstr(&r,keep);
  while (LV_HEAP_BYTES-r.heap_used>2*sizeof(junk))
   lv_text(&r,junk,false); // unrooted garbage from "earlier steps"
  r.step_serial=r.allocation_serial; // the failing step begins here
  lv_t fresh=lv_text(&r,"fresh",false); // unrooted but current-step
  lv_t grown=lv_list(&r,0,NULL,false);
  r.globals[1]=grown;
  for (unsigned i=1;i<=4096 && !r.failed;i++)
   lv_set_at(&r,grown,i,lv_num(i)); // 64 KiB growth needs the emergency pass
  assert(!r.failed);
  assert(lv_count(&r,grown)==4096 && lv_numeric(lv_at(&r,grown,4096))==4096);
  assert(lv_cstr(&r,keep)==keep_raw && !strcmp(keep_raw,"keepsake"));
  assert(!strcmp(lv_cstr(&r,fresh),"fresh"));
  lv_t big=lv_text(&r,junk,false); // allocation failure path, same step
  assert(!r.failed && !strcmp(lv_cstr(&r,big),junk));
  r.globals[0]=(lv_t){0};r.globals[1]=lv_num(5);
  lv_collect(&r); // the between-steps compactor clears the holes
  assert(!r.failed);
 }
 call("undecompiled_bytecode",1,(lv_t[]){lv_text(&r,"unk26",false)});
 assert(r.failed);
 puts("D10 profile bounds, top-channel staging, hover cache, controller window services, FileIO and transition passed");
}
