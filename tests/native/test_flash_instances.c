#include "director.h"
#include "flash_runtime.h"
#include "flash_render.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static dg_runtime_t d;
static lv_runtime_t values;
static const uint8_t stop[]={7,0};
static const dg_flash_action_t child_actions[]={{.frame=2,.length=sizeof(stop),.code=stop}};
static const uint8_t initialize_code[]={0x96,13,0,0,'i','n','i','t','i','a','l','i','z','e','d',0,0x96,2,0,5,1,0x1d,0};
static const dg_flash_action_t initialize_action={.event=0x200,.length=sizeof(initialize_code),.code=initialize_code};
static const dg_film_layer_t root_layers[]={
 {.channel=1,.clip_id=7,.name="left",.action_count=1,.actions=&initialize_action,.placement_generation=1,.affine=true,.matrix={65536,0,0,65536,655360,1310720}},
 {.channel=2,.clip_id=7,.name="right",.placement_generation=2,.affine=true,.matrix={65536,0,0,65536,1966080,1310720}},
 {.channel=3,.asset="red",.mask_asset="red",.clip_depth=4,.value={.x=5,.y=5,.width=2,.height=2}},
 {.channel=4,.asset="blue",.value={.x=4,.y=4,.width=4,.height=4},.has_cxform=true,.cxform={256,256,256,128,0,0,0,0}},
 {.channel=5,.button_id=8,.name="button",.placement_generation=4,.affine=true,.matrix={65536,0,0,65536,2621440,2621440}},
};
static const dg_film_layer_t replacement_layers[]={
 {.channel=1,.clip_id=7,.name="left",.placement_generation=3,.affine=true,.matrix={65536,0,0,65536,655360,1310720}},
 {.channel=2,.clip_id=7,.name="right",.placement_generation=2,.affine=true,.matrix={65536,0,0,65536,1966080,1310720}},
 {.channel=3,.asset="red",.mask_asset="red",.clip_depth=4,.value={.x=5,.y=5,.width=2,.height=2}},
 {.channel=4,.asset="blue",.value={.x=4,.y=4,.width=4,.height=4},.has_cxform=true,.cxform={256,256,256,128,0,0,0,0}},
 {.channel=5,.button_id=8,.name="button",.placement_generation=4,.affine=true,.matrix={65536,0,0,65536,2621440,2621440}},
};
static const dg_film_frame_t root_frames[]={{5,root_layers},{5,root_layers},{5,replacement_layers}};
static const dg_film_layer_t red_layer={.channel=1,.asset="red",.value={.x=-1,.y=-1,.width=2,.height=2}};
static const dg_film_layer_t blue_layer={.channel=1,.asset="blue",.value={.x=-1,.y=-1,.width=2,.height=2}};
static const dg_film_frame_t child_frames[]={{1,&red_layer},{1,&blue_layer},{1,&red_layer}};
static const dg_flash_sound_event_t sound_event={.frame=2,.sound=1,.flags=23,.in_point=1,.out_point=3,.loops=2};
static const dg_flash_sound_t sound={.id=1,.asset="synthetic.wav",.rate=60,.samples=10};
static const dg_flash_clip_t clips[]={
 {.id=0,.frame_count=3,.local_bounds={0,0,4194304,4194304}},
 {.id=7,.frame_count=3,.frames=child_frames,.sound_event_count=1,.sound_events=&sound_event,.action_count=1,.actions=child_actions,.local_bounds={-131072,-196608,524288,655360}},
};
static const uint8_t button_code[]={0x8b,5,0,'l','e','f','t',0,0x81,2,0,2,0,6,0};
static const dg_flash_action_t button_actions[]={{.event=8,.length=sizeof(button_code),.code=button_code}};
static const dg_film_layer_t button_shape={.channel=1,.asset="red",.value={.width=2,.height=2}};
static const dg_flash_button_t buttons[]={{.id=8,.local_bounds={0,0,131072,131072},
 .states={{1,&button_shape},{1,&button_shape},{1,&button_shape},{1,&button_shape}},.action_count=1,.actions=button_actions}};
static const dg_flash_export_t exports[]={{7,"symbol"}};
static const uint8_t small_advances[]={4,5},large_advances[]={7,9};
static const dg_text_style_t small_font={.font_name="SourceFree",.font_id=3,.size=12,.ascent=9,.advances=small_advances,.advance_count=2};
static const dg_text_style_t large_font={.font_name="SourceFree",.font_id=4,.size=16,.ascent=13,.advances=large_advances,.advance_count=2};
static const dg_text_style_t *const font_catalog[]={&small_font,&large_font};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,.width=64,.height=64,
 .film_count=3,.film_loop=1,.source_xtra=1,.film_frames=root_frames,.flash_version=6,
 .flash_font_count=2,.flash_fonts=font_catalog,
 .flash_button_count=1,.flash_buttons=buttons,.flash_sound_count=1,.flash_sounds=&sound,.flash_clip_count=2,.flash_clips=clips,.flash_export_count=1,.flash_exports=exports};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_movie_t movie={.id=1,.cast_count=1,.casts=&cast,.member_count=1,.members=&member};
static lv_t get(dg_flash_instance_t *i,const char *name){return dg_avm1_get(dg_flash_vm(i),dg_flash_object(i),name);}
static void method(dg_flash_instance_t *i,const char *name,unsigned argc,const lv_t *argv) {
 lv_t out;assert(dg_avm1_invoke(dg_flash_vm(i),get(i,name),dg_flash_object(i),argc,argv,100000,&out));assert(!values.failed);
}
static bool hit_asset(void *ctx,const dg_member_t *m,unsigned ink,int x,int y) {
 (void)ctx;assert(ink==8&&x>=0&&y>=0&&x<m->width&&y<m->height);return true;
}
static bool raster_asset(void *ctx,const dg_film_layer_t *layer,dg_flash_raster_t *out) {
 (void)ctx;
 assert(dg_flash_raster_allocate(&d,out,layer->value.x,layer->value.y,layer->value.x+layer->value.width,layer->value.y+layer->value.height));
 uint32_t color=!strcmp(layer->asset,"red")?0xff0000ffu:0xffff0000u;
 for(unsigned n=0;n<out->width*out->height;n++)out->pixels[n]=color;
 return true;
}
static bool raster_text(void *ctx,dg_flash_instance_t *instance,const dg_flash_field_t *field,dg_flash_raster_t *out) {
 (void)ctx;(void)instance;(void)field;(void)out;assert(false);return false;
}
static void pixels(unsigned x,uint32_t first,uint32_t second) {
 dg_flash_raster_host_t host={.scale=1,.asset=raster_asset,.text=raster_text};dg_flash_raster_t out;
 assert(dg_flash_render(&d,1,&host,&out));assert(out.width==64&&out.height==64);
 assert(out.pixels[19*64+x]==first&&out.pixels[19*64+29]==second);
 assert(out.pixels[18*64+x]==0);
 assert(out.pixels[5*64+5]==0x7fff0000u&&out.pixels[6*64+6]==0x7fff0000u);
 assert(!out.pixels[4*64+4]&&!out.pixels[7*64+7]);free(out.pixels);
}
int main(void) {
 dg_init(&d,&values,(dg_platform_t){.hit=hit_asset},NULL,NULL,0,1);d.movie=&movie;d.loaded[0]=&movie;d.loaded_count=1;
 d.sprites[1]=(dg_sprite_t){.value={.type=16,.member=member.id},.visible=true};
 d.sprites[2]=d.sprites[1];d.sprites[2].value.x=100;dg_sprite_changed(&d,1);dg_sprite_changed(&d,2);dg_flash_sync(&d);assert(dg_flash_service(&d,100000));
 dg_flash_instance_t *root=dg_flash_root(&d,1),*other=dg_flash_root(&d,2);
 assert(root&&other&&root!=other);
 dg_flash_instance_t *left=dg_flash_child(root,1),*right=dg_flash_child(root,2);
 assert(left&&right&&left!=right&&get(left,"_parent")==dg_flash_object(root));
 assert(dg_avm1_truth(dg_flash_vm(left),get(left,"initialized"))); // SWF initialize is bit9, not bit14.
 dg_avm1_set(dg_flash_vm(left),dg_flash_object(left),"_yScale",lv_num(100));
 assert(lv_numeric(get(left,"_yscale"))==100&&get(root,"LEFT")==dg_flash_object(left));
 assert(lv_numeric(get(left,"_x"))==10&&lv_numeric(get(right,"_x"))==30);pixels(9,0xff0000ffu,0xff0000ffu);
 method(root,"stop",0,NULL);dg_flash_advance(&d,1,1);assert(!values.failed);
 assert(lv_numeric(get(root,"_currentframe"))==1&&lv_numeric(get(left,"_currentframe"))==2);
 dg_flash_view_t view;assert(dg_flash_view(left,&view)&&!view.playing);pixels(9,0xffff0000u,0xffff0000u);
 assert(dg_flash_audio_count(&d)==1); // SyncNoMultiple suppresses the second same-sound start.
 dg_flash_audio_t voice;assert(dg_flash_audio_at(&d,0,&voice));
 assert(voice.start_sample==1&&voice.end_sample==4&&voice.loops==2&&!voice.position&&voice.id);
 d.ticks=3;assert(dg_flash_audio_at(&d,0,&voice)&&voice.position==3);
 d.ticks=6;assert(dg_flash_audio_count(&d)==0);
 assert(lv_numeric(get(dg_flash_child(other,1),"_currentframe"))==1);
 method(left,"play",0,NULL);dg_flash_advance(&d,1,1);
 assert(lv_numeric(get(left,"_currentframe"))==3&&lv_numeric(get(right,"_currentframe"))==2);pixels(9,0xff0000ffu,0xffff0000u);
 lv_t old_object=dg_flash_object(left);uint32_t old_id=view.id;
 dg_flash_seek(&d,1,2,false);assert(dg_flash_service(&d,100000));
 dg_flash_instance_t *replacement=dg_flash_child(root,1);
 assert(replacement!=left&&dg_flash_view(replacement,&view)&&view.id!=old_id);
 assert(dg_flash_object_instance(&d,old_object)==left&&!dg_flash_view(left,&view));
 assert(dg_flash_child(root,2)==right);
 assert(dg_hit(&d,40,40)==1);
 dg_flash_pointer(&d,40,40,false);dg_flash_pointer(&d,40,40,true);assert(dg_flash_service(&d,100000));
 dg_flash_pointer(&d,40,40,false);assert(dg_flash_service(&d,100000));
 assert(lv_numeric(get(replacement,"_currentframe"))==3);
 assert(dg_flash_view(replacement,&view)&&view.playing);
 lv_t args[]={lv_text(&values,"symbol",false),lv_text(&values,"attached",false),lv_num(100)};
 method(root,"attachMovie",3,args);dg_flash_instance_t *attached=dg_flash_child(root,16484);
 assert(attached&&get(root,"attached")==dg_flash_object(attached));
 assert(dg_flash_layer_count(root)==6);
 dg_flash_layer_view_t layer;assert(dg_flash_layer_at(root,5,&layer)&&layer.instance==attached&&layer.depth==16484);
 dg_avm1_set(dg_flash_vm(attached),dg_flash_object(attached),"_x",lv_num(42));
 assert(lv_numeric(get(attached,"_x"))==42&&lv_numeric(get(right,"_x"))==30);
 // Current display-list bounds come from the visible pose, not the union
 // of every authored frame in the reusable character definition.
 assert(lv_numeric(get(attached,"_width"))==2);
 lv_t duplicate_args[]={lv_text(&values,"copy",false),lv_num(101)};
 method(attached,"duplicateMovieClip",2,duplicate_args);
 dg_flash_instance_t *copy=dg_flash_child(root,16485);
 assert(copy&&lv_numeric(get(copy,"_currentframe"))==1&&lv_numeric(get(copy,"_x"))==42);
 method(attached,"removeMovieClip",0,NULL);assert(!dg_flash_child(root,16484));
 assert(dg_flash_child(root,16485)==copy);
 lv_t empty_args[]={lv_text(&values,"drawing",false),lv_num(102)};
 method(root,"createEmptyMovieClip",2,empty_args);dg_flash_instance_t *drawing=dg_flash_child(root,16486);assert(drawing);
 lv_t fill_args[]={lv_num(0x00ff00),lv_num(100)};method(drawing,"beginFill",2,fill_args);
 lv_t point[]={lv_num(50),lv_num(50)};method(drawing,"moveTo",2,point);
 point[0]=lv_num(54);method(drawing,"lineTo",2,point);point[1]=lv_num(54);method(drawing,"lineTo",2,point);
 point[0]=lv_num(50);method(drawing,"lineTo",2,point);point[1]=lv_num(50);method(drawing,"lineTo",2,point);
 method(drawing,"endFill",0,NULL);assert(lv_numeric(get(drawing,"_width"))==4&&lv_numeric(get(drawing,"_height"))==4);
 dg_flash_raster_host_t host={.scale=1,.asset=raster_asset,.text=raster_text};dg_flash_raster_t raster;
 assert(dg_flash_render(&d,1,&host,&raster));assert(raster.pixels[51*64+51]==0xff00ff00u);free(raster.pixels);
 // Source-free Adobe Flash32 oracle: a 16x100 drawn rectangle rotated90.
 // Assigning width/height resets both scale components using rotated bounds.
 method(drawing,"clear",0,NULL);method(drawing,"beginFill",2,fill_args);
 point[0]=point[1]=lv_num(0);method(drawing,"moveTo",2,point);
 point[0]=lv_num(16);method(drawing,"lineTo",2,point);point[1]=lv_num(100);method(drawing,"lineTo",2,point);
 point[0]=lv_num(0);method(drawing,"lineTo",2,point);point[1]=lv_num(0);method(drawing,"lineTo",2,point);method(drawing,"endFill",0,NULL);
 dg_avm1_t *vm=dg_flash_vm(drawing);lv_t object=dg_flash_object(drawing);
 dg_avm1_set(vm,object,"_rotation",lv_num(90));dg_avm1_set(vm,object,"_xscale",lv_num(80));dg_avm1_set(vm,object,"_yscale",lv_num(-80));
 assert(fabs(lv_numeric(get(drawing,"_width"))-80)<0.01&&fabs(lv_numeric(get(drawing,"_height"))-12.8)<0.01);
 dg_avm1_set(vm,object,"_width",lv_num(40));
 assert(fabs(lv_numeric(get(drawing,"_xscale"))-40)<0.01&&fabs(lv_numeric(get(drawing,"_yscale"))-80)<0.01);
 dg_avm1_set(vm,object,"_height",lv_num(20));
 assert(fabs(lv_numeric(get(drawing,"_xscale"))-80)<0.01&&fabs(lv_numeric(get(drawing,"_yscale"))-125)<0.01);
 dg_avm1_set(vm,object,"_xscale",lv_num(-100));dg_avm1_set(vm,object,"_xscale",lv_num(-100));
 assert(fabs(lv_numeric(get(drawing,"_xscale"))+100)<0.01&&fabs(lv_numeric(get(drawing,"_rotation"))-90)<0.01);
 method(drawing,"removeMovieClip",0,NULL);
 lv_t field_args[]={lv_text(&values,"label",false),lv_num(103),lv_num(2),lv_num(3),lv_num(40),lv_num(20)};
 method(root,"createTextField",6,field_args);dg_flash_instance_t *field=dg_flash_child(root,16487);assert(field);
 assert(lv_numeric(get(field,"_x"))==2&&lv_numeric(get(field,"_width"))==40);
 assert(!strcmp(lv_cstr(&values,get(field,"font")),"Times New Roman"));
 dg_avm1_set(dg_flash_vm(field),dg_flash_object(field),"text",lv_text(&values,"hello",false));
 assert(!strcmp(lv_cstr(&values,get(field,"text")),"hello"));
 dg_avm1_set(dg_flash_vm(field),dg_flash_object(field),"font",lv_text(&values,"SourceFree",false));
 dg_avm1_set(dg_flash_vm(field),dg_flash_object(field),"size",lv_num(16));
 assert(dg_flash_view(field,&view)&&view.field);
 dg_flash_field_t captured_field;dg_text_style_t captured_style;
 assert(dg_flash_field_snapshot(&d,field,view.field,&captured_field,&captured_style));
 assert(captured_style.font_id==4&&captured_style.size==16&&captured_style.ascent==13);
 assert(captured_style.advances==large_advances&&captured_style.advances[1]==9);
 dg_avm1_set(dg_flash_vm(field),dg_flash_object(field),"font",lv_text(&values,"MissingFace",false));
 assert(!dg_flash_field_snapshot(&d,field,view.field,&captured_field,&captured_style));
 assert(values.failed&&strstr(values.error,"font inventory"));values.failed=false;values.error[0]=0;
 method(field,"removeTextField",0,NULL);assert(!dg_flash_child(root,16487));
 d.sprites[1].value.member=0;dg_flash_sync(&d);assert(!dg_flash_root(&d,1)&&dg_flash_root(&d,2));
 dg_flash_dispose(&d);assert(!d.flash_state&&!values.external_roots);
 puts("Flash instance timelines, replacement generations, rooted objects and dynamic depths pass");
}
