// Source-free display-list churn, external removed-object references and unload jobs.
#include "director.h"
#include "flash_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t v;
static unsigned unloads;
static const lv_movie_t code={.name="FLASH-LIFETIME.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t score={0};
static const dg_film_frame_t empty={0};
static const uint8_t unload_code[]={0x96,8,0,0,'u','n','l','o','a','d',0,0x26,0};
static const dg_flash_action_t unload={.event=4,.length=sizeof(unload_code),.code=unload_code};
static const dg_flash_clip_t clips[]={
 {.id=0,.frame_count=1,.frames=&empty},
 {.id=7,.frame_count=1,.frames=&empty,.action_count=1,.actions=&unload}};
static const dg_flash_export_t exports[]={{7,"symbol"}};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,
 .width=8,.height=8,.film_count=1,.film_frames=&empty,.source_xtra=1,.flash_version=6,
 .flash_clip_count=2,.flash_clips=clips,.flash_export_count=1,.flash_exports=exports};
static const dg_movie_t movie={.id=1,.code=&code,.cast_count=1,.casts=&cast,
 .member_count=1,.members=&member,.frame_count=1,.frames=&score};
static void trace(void *ctx,const char *message){(void)ctx;assert(!strcmp(message,"unload"));unloads++;}
static lv_t invoke(dg_avm1_t *vm,lv_t target,const char *name,unsigned argc,const lv_t *args){
 lv_t out;assert(dg_avm1_invoke(vm,dg_avm1_get(vm,target,name),target,argc,args,100000,&out));
 if(v.failed)fprintf(stderr,"%s\n",v.error);
 assert(!v.failed);
 return out;
}
int main(void){
 dg_init(&d,&v,(dg_platform_t){.trace=trace},NULL,NULL,0,1);assert(dg_enter(&d,&movie,1,NULL,0));
 d.sprites[1]=(dg_sprite_t){.value={.member=member.id,.type=16},.visible=true};
 dg_sprite_changed(&d,1);dg_flash_sync(&d);assert(dg_flash_service(&d,100000));
 dg_flash_instance_t *root=dg_flash_root(&d,1);dg_avm1_t *vm=dg_flash_vm(root);lv_t object=dg_flash_object(root);
 lv_t args[]={lv_text(&v,"symbol",false),lv_text(&v,"temporary",false),lv_num(100)};
 lv_root_span_t arguments={.next=v.external_roots,.values=args,.count=3};v.external_roots=&arguments;
 lv_t kept=invoke(vm,object,"attachMovie",3,args);v.roots[0]=kept;
 dg_avm1_own_set(vm,kept,"marker",lv_num(73));
 dg_flash_instance_t *retired=dg_flash_object_instance(&d,kept);assert(retired);
 invoke(vm,kept,"removeMovieClip",0,NULL);
 // A pending unload job is serviced before its native receiver is reclaimed.
 for(unsigned n=0;n<64;n++){
  lv_t child=invoke(vm,object,"attachMovie",3,args);invoke(vm,child,"removeMovieClip",0,NULL);
 }
 assert(!unloads);assert(dg_flash_service(&d,1));assert(unloads<65);
 assert(dg_flash_service(&d,100000));assert(unloads==65);
 for(unsigned n=0;n<5000;n++){
  lv_t child=invoke(vm,object,"attachMovie",3,args);invoke(vm,child,"removeMovieClip",0,NULL);
  assert(dg_flash_service(&d,100000));
 }
 assert(unloads==5065&&!v.failed&&!dg_flash_layer_count(root));
 dg_flash_view_t view;
 assert(dg_flash_object_instance(&d,kept)==retired&&!dg_flash_view(retired,&view));
 assert(!dg_avm1_get(vm,kept,"_x"));lv_t marker;
 assert(dg_avm1_own_get(vm,kept,"marker",&marker)&&lv_numeric(marker)==73);
 // Releasing the script reference makes the same native allocation eligible.
 v.roots[0]=0;
 for(unsigned n=0;n<64;n++){
  lv_t child=invoke(vm,object,"attachMovie",3,args);invoke(vm,child,"removeMovieClip",0,NULL);
  assert(dg_flash_service(&d,100000));
 }
 // Once released, the old LV handle may be recycled for a different object.
 if(dg_flash_object_instance(&d,kept))
   assert(!dg_avm1_own_get(vm,kept,"marker",&marker));
 lv_root_span_t **span=&v.external_roots;
 while(*span&&*span!=&arguments)span=&(*span)->next;
 assert(*span==&arguments);*span=arguments.next;
 dg_flash_dispose(&d);dg_films_dispose(&d);
 puts("Flash lifetime: churn, pending unloads and persistent removed-object identity passed");return 0;
}
