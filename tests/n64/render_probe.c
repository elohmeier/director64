// Synthetic N64 pixel contracts. No game assets; built by tools/reference/n64-render-probe.py.
#define main director_game_main
#include "director_main.c"
#undef main
#include "palette.h"
bool text_input_active(void){return false;}
bool d5_video_dimensions(const dg_runtime_t *d,unsigned i,unsigned *w,unsigned *h){(void)d;(void)i;(void)w;(void)h;assertf(false,"unexpected video");return false;}
void d5_video_draw(const dg_runtime_t *d,unsigned i,int l,int t,int r,int b){(void)d;(void)i;(void)l;(void)t;(void)r;(void)b;assertf(false,"unexpected video");}
static const lv_movie_t test_code={.name="SYNTHETIC.DXR"};
static const dg_cast_t test_cast={.name="Internal",.file=1,.cast=1};
static const dg_text_style_t test_style={.color=0xff0000,.size=12,.ascent=12,.line_height=12};
static const dg_film_layer_t test_layers[]={{.channel=1,.value={.member=DG_ID(1,1,1),.type=16,.width=4,.height=4,.ink=8,.blend=100,.thickness=DG_HAS_BLEND,.flags=48,.back_rgb=0xffffff,.stretch=1}}};
static const dg_film_frame_t test_film={1,test_layers};
static const dg_film_frame_t flash_frame={0,NULL};
static const dg_flash_clip_t flash_clip={.id=0,.frame_count=1,.local_bounds={0,0,327680,327680}};
static const dg_film_layer_t preload_leaf[]={{.channel=1,.value={.member=DG_ID(1,1,6),.type=16,.width=5,.height=5,.ink=9,.blend=100,.thickness=DG_HAS_BLEND,.stretch=1}}};
static const dg_film_frame_t preload_inner={1,preload_leaf};
static const dg_film_layer_t preload_nested[]={{.channel=2,.value={.member=DG_ID(1,1,8),.type=16,.width=5,.height=5,.ink=8,.blend=100,.thickness=DG_HAS_BLEND,.stretch=1}}};
static const dg_film_frame_t preload_outer={1,preload_nested};
static const dg_film_layer_t pattern_layers[]={{.channel=1,.value={.member=DG_ID(1,1,10),.type=16,.x=5,.y=7,.width=30,.height=40,.ink=8,.blend=100,.thickness=DG_HAS_BLEND,.flags=48,.back_rgb=0xffffff,.stretch=1}}};
static const dg_film_frame_t pattern_frame={1,pattern_layers};
static const dg_member_t test_members[]={
 {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=1,.width=4,.height=4},
 {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=3,.width=20,.height=16,.text="A",.text_style=&test_style},
 {.id=DG_ID(1,1,3),.number=3,.cast=1,.type=2,.width=4,.height=4,.film_count=1,.film_frames=&test_film},
 {.id=DG_ID(1,1,4),.number=4,.cast=1,.type=2,.source_xtra=1,.width=5,.height=5,
  .film_count=1,.film_frames=&flash_frame,.flash_version=6,.flash_clip_count=1,.flash_clips=&flash_clip},
 {.id=DG_ID(1,1,5),.number=5,.cast=1,.type=8,.width=5,.height=5,
  .shape=1,.filled=true,.pattern=58,.asset="synthetic-brush.fdi"},
 {.id=DG_ID(1,1,6),.number=6,.cast=1,.type=1,.width=4,.height=4,.name="source",.asset="preload-source.fdi"},
 {.id=DG_ID(1,1,7),.number=7,.cast=1,.type=1,.width=4,.height=4,.name="mask",.asset="preload-mask.fdi"},
 {.id=DG_ID(1,1,8),.number=8,.cast=1,.type=2,.width=5,.height=5,.film_count=1,.film_frames=&preload_inner},
 {.id=DG_ID(1,1,9),.number=9,.cast=1,.type=2,.width=5,.height=5,.film_count=1,.film_frames=&preload_outer},
 {.id=DG_ID(1,1,10),.number=10,.cast=1,.type=8,.width=30,.height=40,.shape=1,.filled=true,.pattern=58,.asset="synthetic-pattern.fd2"},
 {.id=DG_ID(1,1,11),.number=11,.cast=1,.type=2,.width=80,.height=80,.film_count=1,.film_frames=&pattern_frame},
 {.id=DG_ID(1,1,12),.number=12,.cast=1,.type=1,.width=800,.height=600,.asset="resident-background.fdi"},
 {.id=DG_ID(1,1,13),.number=13,.cast=1,.type=1,.width=320,.height=200,.asset="prefix-source.fdi"},
 {.id=DG_ID(1,1,14),.number=14,.cast=1,.type=1,.width=320,.height=200,.asset="prefix-mask.fdi"},
};
static const dg_frame_t test_frame={0,0};
static const dg_movie_t test_movie={.code=&test_code,.id=1,.tempo=30,.cast_count=1,.casts=&test_cast,.members=test_members,.member_count=14,.frame_count=1,.frames=&test_frame};
static unsigned failed;
static void *volatile fixture_pressure;
static void check_pixel(const char *name,uint32_t actual,uint32_t expected){
 if(actual!=expected)failed++;
 debugf("DIRECTOR64 FILM_PIXEL %s actual=%08x expected=%08x\n",name,(unsigned)actual,(unsigned)expected);
}
static void flash_method(dg_flash_instance_t *instance,const char *name,unsigned count,const lv_t *args){
 dg_avm1_t *vm=dg_flash_vm(instance);lv_t object=dg_flash_object(instance),result;
 assertf(dg_avm1_invoke(vm,dg_avm1_get(vm,object,name),object,count,args,100000,&result),"synthetic Flash method failed: %s",name);
}
static surface_t glyph;
static unsigned glyph_overlap;
static void draw_glyph(void *ctx,int x,int y){
 (void)ctx;rdpq_set_mode_standard();rdpq_mode_dithering(DITHER_NONE_NONE);
 rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);rdpq_mode_alphacompare(1);
 rdpq_set_prim_color(RGBA32(255,0,0,255));
 rdpq_mode_combiner(RDPQ_COMBINER1((0,0,0,PRIM),(TEX0,0,PRIM,0)));
 text_capture_mode((void *)(uintptr_t)text_capture);
 rdpq_tex_blit(&glyph,x,y,NULL);
 if(glyph_overlap)rdpq_tex_blit(&glyph,x,y,NULL);
}
static void prefix_draw(surface_t *target,bool cached,unsigned cursor_x) {
 completed_serial=render_serial;render_serial++;image_streaming=false;
 dg_sprite_changed(director,1);dg_sprite_changed(director,2);
 scene_prefix_context=cached?director:NULL;
 rdpq_attach(target,NULL);draw_scene(target,director,0,0);
 // A pointer is composited after the scene and never enters the prefix.
 rdpq_set_mode_fill(RGBA32(0,255,0,255));rdpq_fill_rectangle(cursor_x,450,cursor_x+4,454);
 rdpq_detach_wait();scene_prefix_context=NULL;
}
int main(void){
 debug_init_isviewer();assert_memory_expanded();rdpq_init();
 values=calloc(1,sizeof(*values));director=calloc(1,sizeof(*director));
 dg_init(director,values,(dg_platform_t){0},NULL,NULL,0,1);assertf(dg_enter(director,&test_movie,1,NULL,0),"synthetic enter: %s",values->error);
 glyph=surface_alloc(FMT_RGBA32,4,1);
 for(unsigned i=0;i<4;i++)((uint32_t*)glyph.buffer)[i]=0xffffff80;
 surface_t captured=capture_text_plane(4,1,draw_glyph,NULL);
 check_pixel("single_glyph_coverage",((uint32_t*)captured.buffer)[0],0xff00007f);
 surface_free(&captured);glyph_overlap=1;
 captured=capture_text_plane(4,1,draw_glyph,NULL);
 check_pixel("overlapping_glyph_coverage",((uint32_t*)captured.buffer)[0],0xff0000bf);
 surface_free(&captured);surface_free(&glyph);
 font=rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);rdpq_text_register_font(1,font);
 surface_t source=surface_alloc(FMT_RGBA32,4,4),film=surface_alloc(FMT_RGBA32,4,4),fb=surface_alloc(FMT_RGBA16,4,4);
 for(unsigned i=0;i<16;i++)((uint32_t*)source.buffer)[i]=0xff000080;
 memset(film.buffer,0,film.stride*film.height);
 uint8_t header[32]={0};cached_image_t image={.data=header,.format=IMAGE_RGBA32,.follow_alpha=true};
 dg_sprite_t sprite={.value={.member=DG_ID(1,1,1),.type=16,.width=5,.height=5,.blend=100,.thickness=DG_HAS_BLEND,.flags=48,.back_rgb=0xffffff},.visible=true,.stretch=true};
 rdpq_attach(&film,NULL);ink_bitmap(&film,&source,NULL,&image,&sprite,0,0,4,4,8,100,false,NULL);rdpq_detach_wait();
 check_pixel("transparent_film_keeps_rgb_and_alpha",((uint32_t*)film.buffer)[0],0xff000080);
 for(unsigned i=0;i<16;i++)((uint16_t*)fb.buffer)[i]=0x003f;
 rdpq_attach(&fb,NULL);ink_bitmap(&fb,&film,NULL,&image,&sprite,0,0,4,4,8,100,false,NULL);rdpq_detach_wait();
 check_pixel("film_over_blue",((uint16_t*)fb.buffer)[0],0x801f);
 // The second CPU draw must observe intervening RDP writes rather than its
 // earlier cached destination. A subsequent RDP read must see the CPU result.
 rdpq_attach(&fb,NULL);rdpq_set_mode_fill(RGBA32(0,255,0,255));rdpq_fill_rectangle(0,0,4,4);
 ink_bitmap(&fb,&film,NULL,&image,&sprite,0,0,4,4,8,100,false,NULL);rdpq_detach_wait();
 check_pixel("cached_cpu_observes_rdp_destination",((uint16_t*)fb.buffer)[0],0x83c1);
 surface_t copied=surface_alloc(FMT_RGBA16,4,4);
 rdpq_attach(&copied,NULL);rdpq_set_mode_standard();rdpq_mode_filter(FILTER_POINT);
 rdpq_tex_blit(&fb,0,0,NULL);rdpq_detach_wait();
 check_pixel("rdp_observes_cached_cpu_writeback",((uint16_t*)copied.buffer)[0],0x83c1);
 surface_free(&copied);
 for(unsigned i=0;i<16;i++)((uint32_t*)source.buffer)[i]=0x00ff0080;
 rdpq_attach(&film,NULL);ink_bitmap(&film,&source,NULL,&image,&sprite,0,0,4,4,8,100,false,NULL);rdpq_detach_wait();
 check_pixel("overlapping_translucent_layers",((uint32_t*)film.buffer)[0],0x55aa00c0);
 lv_frame_t frame={.movie=&test_code};
 lv_t img=lv_call(values,&frame,"image",3,(lv_t[]){lv_num(4),lv_num(4),lv_num(32)});
 lv_call(values,&frame,"fill",3,(lv_t[]){img,lv_rect(values,0,0,4,4),lv_make(LV_COLOR,0xff0000)});
 lv_call(values,&frame,"setalpha",2,(lv_t[]){img,lv_num(128)});
 lv_set(values,NULL,"usealpha",img,lv_num(1));
 lv_set(values,NULL,"image",lv_make(LV_MEMBER,DG_ID(1,1,1)),img);
 lv_set(values,NULL,"regpoint",lv_make(LV_MEMBER,DG_ID(1,1,1)),lv_point(values,0,0));
 assertf(!values->failed,"synthetic image setup: %s",values->error);
 director->sprites[1]=sprite;director->sprites[1].value.member=DG_ID(1,1,3);
 director->sprites[1].value.width=director->sprites[1].value.height=4;
 director->stage_color=0x0000ff;dg_sprite_changed(director,1);
 heap_stats_t before,after;sys_get_heap_stats(&before);
 uint64_t started=get_ticks_us();
 rdpq_attach(&fb,NULL);draw_scene(&fb,director,0,0);rdpq_detach_wait();
 uint64_t elapsed=get_ticks_us()-started;sys_get_heap_stats(&after);
 debugf("DIRECTOR64 FILM_COST us=%llu heap_before=%d heap_after=%d\n",(unsigned long long)elapsed,before.free,after.free);
 assertf(!values->failed,"synthetic film: %s",values->error);
 check_pixel("actual_film_child_over_blue",((uint16_t*)fb.buffer)[0],0x801f);
 director->sprites[1].value.member=DG_ID(1,1,4);
 director->sprites[1].value.width=director->sprites[1].value.height=5;
 dg_sprite_changed(director,1);dg_flash_sync(director);
 assertf(dg_flash_service(director,100000),"synthetic Flash bootstrap: %s",values->error);
 dg_flash_instance_t *root=dg_flash_root(director,1);
 lv_t args[]={lv_text(values,"drawing",false),lv_num(1)};
 flash_method(root,"createEmptyMovieClip",2,args);
 dg_flash_instance_t *child=dg_flash_child(root,16385);
 args[0]=lv_num(0xff0000);args[1]=lv_num(100);flash_method(child,"beginFill",2,args);
 args[0]=args[1]=lv_num(0);flash_method(child,"moveTo",2,args);
 args[0]=lv_num(5);flash_method(child,"lineTo",2,args);
 args[1]=lv_num(5);flash_method(child,"lineTo",2,args);
 args[0]=lv_num(0);flash_method(child,"lineTo",2,args);
 args[1]=lv_num(0);flash_method(child,"lineTo",2,args);
 flash_method(child,"endFill",0,NULL);
 dg_avm1_set(dg_flash_vm(child),dg_flash_object(child),"_alpha",lv_num(50));
 dg_flash_raster_host_t host={.scale=0.8f,.asset=n64_flash_asset,.text=n64_flash_text};
 dg_flash_raster_t flash_pixels;
 assertf(dg_flash_render(director,1,&host,&flash_pixels),"synthetic Flash raster");
 check_pixel("flash_dynamic_plane",flash_pixels.pixels[0],0x7f0000ff);
 free(flash_pixels.pixels);
 sys_get_heap_stats(&before);started=get_ticks_us();
 rdpq_attach(&fb,NULL);draw_scene(&fb,director,0,0);rdpq_detach_wait();
 elapsed=get_ticks_us()-started;sys_get_heap_stats(&after);
 debugf("DIRECTOR64 FILM_FLASH_COST us=%llu heap_before=%d heap_after=%d\n",(unsigned long long)elapsed,before.free,after.free);
 check_pixel("actual_flash_child_over_blue",((uint16_t*)fb.buffer)[0],0x7821);
 // Original DIRAPI8.5.1r104 shape observations: red over blue at10/50/99%.
 // The N64 retains the exact observed8-bit arithmetic before RGB555 storage.
 dg_sprite_t fade_saved=director->sprites[1];
 surface_t fades_target=surface_alloc(FMT_RGBA16,32,2);
 const unsigned fade_levels[]={10,50,99},fade_expected[]={0x1839,0x7821,0xf801};
 const unsigned fade_inks[]={0,8,32};unsigned fade_errors=0;
 director->sprites[1]=(dg_sprite_t){.value={.type=1,.x=20,.y=1,.width=40,.height=2,
     .thickness=DG_HAS_BLEND|3,.flags=48,.fore_rgb=0xff0000,.back_rgb=0xffffff},.visible=true,.stretch=true};
 for(unsigned ink=0;ink<3;ink++)for(unsigned level=0;level<3;level++) {
   director->sprites[1].value.ink=fade_inks[ink];director->sprites[1].value.blend=fade_levels[level];
   dg_sprite_changed(director,1);rdpq_attach(&fades_target,NULL);draw_scene(&fades_target,director,0,0);rdpq_detach_wait();
   for(unsigned x=0;x<32;x++)if(((uint16_t*)fades_target.buffer)[x]!=fade_expected[level])fade_errors++;
 }
 check_pixel("shape_fade_original_10_50_99",fade_errors,0);surface_free(&fades_target);
 director->sprites[1]=fade_saved;
 // Differential check against the prior per-pixel shape channel evaluation:
 // all32 display channel levels, varied inks, geometry, outline and clipping.
 surface_t span_target=surface_alloc(FMT_RGBA16,8,8);unsigned span_errors=0;
 const unsigned span_inks[]={2,6,33,35,36,40,41};
 const unsigned span_opacity[]={1,33,50,99};
 uint32_t saved_background=director->stage_color;
 director->dynamic_count=1;
 for(unsigned geometry=0;geometry<6;geometry++)for(unsigned ink=0;ink<7;ink++)
   for(unsigned level=0;level<4;level++)for(unsigned ramp=0;ramp<4;ramp++) {
     unsigned red=(geometry*5+ramp*8)&31,green=(red+11)&31,blue=(red+23)&31;
     director->stage_color=((red<<3|red>>2)<<16)|((green<<3|green>>2)<<8)|(blue<<3|blue>>2);
     director->dynamic_members[0].member=(dg_member_t){.id=DG_ID(1,1,5),.type=8,.shape=geometry%3+1,
         .width=12,.height=10,.filled=geometry<3,.pattern=geometry&1,.line_direction=6};
     dg_member_t *shape=&director->dynamic_members[0].member;
     director->sprites[1]=(dg_sprite_t){.value={.type=16,.member=shape->id,.x=-2,.y=-1,.width=12,.height=10,
         .thickness=DG_HAS_BLEND|3,.ink=span_inks[ink],.blend=span_opacity[level],.flags=48,
         .fore_rgb=0x14283c,.back_rgb=0xdcb48c},.visible=true,.stretch=true};
     dg_sprite_changed(director,1);rdpq_attach(&span_target,NULL);draw_scene(&span_target,director,0,0);rdpq_detach_wait();
     int left,top,right,bottom;dg_sprite_bounds(director,&director->sprites[1],&left,&top,&right,&bottom);
     left=dg_prescale_coord(left);top=dg_prescale_coord(top);right=dg_prescale_coord(right);bottom=dg_prescale_coord(bottom);
     for(int y=0;y<8;y++)for(int x=0;x<8;x++) {
       unsigned expected=(red<<11)|(green<<6)|(blue<<1)|1;
       if(x>=left&&x<right&&y>=top&&y<bottom&&
          dg_shape_contains(shape->shape,right-left,bottom-top,x-left,y-top,2,shape->filled,true)&&
          dg_shape_ink_visible(true,span_inks[ink],span_opacity[level],0x3c2814,0x8cb4dc)) {
         unsigned rr=dg_shape_ink_channel(true,red<<3|red>>2,span_inks[ink],span_opacity[level],20,220);
         unsigned gg=dg_shape_ink_channel(true,green<<3|green>>2,span_inks[ink],span_opacity[level],40,180);
         unsigned bb=dg_shape_ink_channel(true,blue<<3|blue>>2,span_inks[ink],span_opacity[level],60,140);
         expected=(rr>>3<<11)|(gg>>3<<6)|(bb>>3<<1)|1;
       }
       if(((uint16_t*)((uint8_t*)span_target.buffer+y*span_target.stride))[x]!=expected)span_errors++;
     }
   }
 check_pixel("shape_span_full_pixel_differential",span_errors,0);surface_free(&span_target);
 director->dynamic_count=0;memset(&director->dynamic_members[0],0,sizeof(director->dynamic_members[0]));director->stage_color=saved_background;director->sprites[1]=fade_saved;
 // The indexed brush keeps its index through the image cache and resolves
 // it against the live stage palette in the actual shape draw branch.
 uint8_t brush[545]={0};memcpy(brush,"FDIC",4);
 brush[6]=2;brush[7]=33;brush[9]=brush[11]=1;
 brush[16]=4;brush[18]=brush[19]=8;brush[20]=1;brush[544]=249;
 cache[0]=(cached_image_t){.data=brush,.bytes=sizeof(brush),.format=IMAGE_CI8,.colors_m1=255};
 strcpy(cache[0].name,"synthetic-brush.fdi");
 cache_key[0]=dg_image_key(cache[0].name,0);
 director->sprites[1].value.member=DG_ID(1,1,5);
 director->sprites[1].value.ink=0;
 dg_sprite_changed(director,1);
 director->palette_active=true;dg_system_palette(director->active_palette,false);
 rdpq_attach(&film,NULL);draw_scene(&film,director,0,0);rdpq_detach_wait();
 check_pixel("shape_index_mac",((uint32_t*)film.buffer)[0],0x888888ff);
 dg_system_palette(director->active_palette,true);
 rdpq_attach(&film,NULL);draw_scene(&film,director,0,0);rdpq_detach_wait();
 check_pixel("shape_index_win",((uint32_t*)film.buffer)[0],0x008080ff);
 // Nested live films contribute both the source and its adjacent mask
 // before any renderer pins the stage's image set.
 image_pending_count=0;image_pending_bytes=image_pending_temporary=0;
 dg_sprite_t nested={.value={.member=DG_ID(1,1,9),.type=16,.width=5,.height=5,.blend=100,.thickness=DG_HAS_BLEND},.visible=true,.stretch=true};
 image_pending_sprite(director,DG_SPRITES,&nested,NULL,0,0,0,5,5,0);
 check_pixel("nested_preload_with_mask",image_pending_count,2);
 check_pixel("nested_preload_temporaries",image_pending_temporary,128);
 // Streaming may release a source only after queued RDP texture reads have
 // finished. The framebuffer must retain the exact result after eviction.
 memset(&cache[0],0,sizeof(cache[0]));cache_key[0]=0;
 cache[0].data=memalign(32,64);assertf(cache[0].data,"streaming source allocation");
 for(unsigned i=0;i<16;i++)((uint32_t*)cache[0].data)[i]=0xff0000ff;
 data_cache_hit_writeback(cache[0].data,64);
 cache[0].bytes=64;cache[0].last=1;cache_bytes=cache_arena_bytes=64;
 render_serial=1;completed_serial=0;image_streaming=false;
 surface_t streaming=surface_make_linear(cache[0].data,FMT_RGBA32,4,4);
 rdpq_attach(&fb,NULL);rdpq_set_mode_standard();rdpq_mode_dithering(DITHER_NONE_NONE);
 rdpq_tex_blit(&streaming,0,0,NULL);
 check_pixel("queued_image_remains_pinned",evict(),0);
 image_streaming=true;image_stream_boundary();
 check_pixel("completed_image_is_evictable",evict(),1);
 rdpq_detach_wait();check_pixel("streamed_pixel_after_source_release",((uint16_t*)fb.buffer)[0],0xf801);
 sys_get_heap_stats(&before);
 unsigned occupied=(unsigned)before.free-256u*1024u;
 cache[0].data=memalign(32,occupied);assertf(cache[0].data,"temporary pressure fixture");
 cache[0].bytes=occupied;cache[0].last=0;cache_bytes=cache_arena_bytes=occupied;
 surface_t reclaimed=image_temporary_surface(FMT_RGBA32,512,512);
 check_pixel("film_temporary_reclaims_completed_images",reclaimed.buffer!=NULL && cache[0].data==NULL,1);
 if(reclaimed.buffer)surface_free(&reclaimed);
 // Reclaim cheap completed planes before an expensive recent background,
 // while keeping an in-flight cheap plane pinned and preferring stale art.
 render_serial=completed_serial=10;image_streaming=true;
 for(unsigned i=0;i<3;i++) {
   cache[i]=(cached_image_t){.data=malloc(64),.bytes=64,.last=i==2?11:10};
   cache_reload_us[i]=i==0?10000:i==1?100:1;cache_bytes+=64;cache_arena_bytes+=64;
 }
 check_pixel("streaming_evicts_cheapest_completed_plane",evict()&&!cache[1].data&&cache[0].data&&cache[2].data,1);
 cache[1]=(cached_image_t){.data=malloc(64),.bytes=64,.last=0};
 cache_reload_us[1]=100000;cache_bytes+=64;cache_arena_bytes+=64;
 check_pixel("streaming_evicts_stale_before_expensive_recent",evict()&&!cache[1].data&&cache[0].data&&cache[2].data,1);
 free(cache[0].data);free(cache[2].data);
 memset(cache,0,sizeof(cache));memset(cache_reload_us,0,sizeof(cache_reload_us));
 cache_bytes=cache_arena_bytes=0;render_serial=completed_serial=0;image_streaming=false;

 // Fractional authored coordinates and both flips exercise exact sample
 // phase. Compare every output pixel to the unchanged general quad inverse.
 surface_t samples=surface_alloc(FMT_RGBA32,8,8),sampled=surface_alloc(FMT_RGBA32,12,10);
 for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)
   ((uint32_t*)samples.buffer)[y*8+x]=(x*31u)<<24|(y*31u)<<16|0x55ff;
 dg_sprite_t fractional={.value={.type=16,.blend=100,.flags=48,.back_rgb=0xffffff},
   .visible=true,.custom_quad=true,.quad={0.3f,0.7f,13.7f,0.7f,13.7f,11.3f,0.3f,11.3f}};
 unsigned sampling_errors=0;
 for(unsigned flip=0;flip<4;flip++) {
   fractional.flip_h=flip&1;fractional.flip_v=flip&2;
   for(unsigned i=0;i<120;i++)((uint32_t*)sampled.buffer)[i]=0x0000ffff;
   rdpq_attach(&sampled,NULL);ink_bitmap(&sampled,&samples,NULL,&image,&fractional,0,0,12,10,8,100,false,NULL);rdpq_detach_wait();
   float q[8];dg_sprite_quad(director,&fractional,q);for(unsigned i=0;i<8;i++)q[i]*=0.8f;
   for(unsigned y=0;y<10;y++)for(unsigned x=0;x<12;x++) {
     float u,v;uint32_t expected=0x0000ffff;
     if(dg_quad_uv(q,x+0.5f,y+0.5f,&u,&v)) {
       if(fractional.flip_h)u=1-u;
       if(fractional.flip_v)v=1-v;
       unsigned sx=u*8,sy=v*8;if(sx>7)sx=7;if(sy>7)sy=7;
       expected=((uint32_t*)samples.buffer)[sy*8+sx];
     }
     if(((uint32_t*)sampled.buffer)[y*12+x]!=expected)sampling_errors++;
   }
 }
 check_pixel("axis_sampling_matches_general_quad",sampling_errors,0);
 surface_free(&sampled);surface_free(&samples);
 // Cropped intermediates keep full-plane sampling and mask registration.
 // Compare full versus cropped surfaces at fractional positions and flips.
 surface_t full=surface_alloc(FMT_RGBA32,8,8),cropped=surface_alloc(FMT_RGBA32,4,4);
 surface_t reference=surface_alloc(FMT_RGBA32,12,10),optimized=surface_alloc(FMT_RGBA32,12,10);
 memset(full.buffer,0,full.stride*full.height);
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++) {
   uint32_t value=((x+2)*31u)<<24|((y+1)*31u)<<16|0x55ff;
   ((uint32_t*)full.buffer)[(y+1)*8+x+2]=value;
   ((uint32_t*)cropped.buffer)[y*4+x]=value;
 }
 uint8_t *mask_data=calloc(1,64);assertf(mask_data,"mask fixture allocation");
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)
   ((uint16_t*)(mask_data+32))[y*4+x]=(x+y)%3==0?0xffff:(x+y)%3==1?0:0x8421;
 cache[0]=(cached_image_t){.data=mask_data,.bytes=64,.format=IMAGE_RGBA16,.ink=8};
 strcpy(cache[0].name,"preload-mask.fdi");cache_key[0]=dg_image_key(cache[0].name,8);
 cache_bytes=cache_arena_bytes=64;
 unsigned crop_errors=0;bitmap_region_t region={8,8,2,1};
 for(unsigned mask_ink=0;mask_ink<2;mask_ink++)for(unsigned flip=0;flip<4;flip++) {
   fractional.value.member=DG_ID(1,1,6);fractional.value.ink=mask_ink?9:8;
   fractional.flip_h=flip&1;fractional.flip_v=flip&2;
   for(unsigned i=0;i<120;i++)((uint32_t*)reference.buffer)[i]=((uint32_t*)optimized.buffer)[i]=0x0000ffff;
   rdpq_attach(&reference,NULL);ink_bitmap(&reference,&full,NULL,&image,&fractional,0,0,12,10,fractional.value.ink,100,false,NULL);rdpq_detach_wait();
   rdpq_attach(&optimized,NULL);ink_bitmap_region(&optimized,&cropped,NULL,&image,&fractional,0,0,12,10,fractional.value.ink,100,false,NULL,&region);rdpq_detach_wait();
   for(unsigned i=0;i<120;i++)if(((uint32_t*)reference.buffer)[i]!=((uint32_t*)optimized.buffer)[i])crop_errors++;
 }
 check_pixel("cropped_fractional_flip_parent_mask_equivalence",crop_errors,0);
 surface_t mask_words=surface_alloc(FMT_RGBA16,4,4),mask_output=surface_alloc(FMT_RGBA16,4,4);
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++) {
   ((uint16_t*)mask_words.buffer)[y*4+x]=0xf800u|((x+y)&1u);
   ((uint16_t*)(mask_data+32))[y*4+x]=x%3==0?0xffff:x%3==1?1:0x8421;
 }
 dg_sprite_t mask_sprite={.value={.member=DG_ID(1,1,6),.type=16,.width=5,.height=5,.ink=9,.blend=100,.flags=48,.back_rgb=0xffffff},.visible=true,.stretch=true};
 cached_image_t word_image={.data=header,.format=IMAGE_RGBA16};unsigned mask_word_errors=0;
 unsigned mask_mapping_errors=0;
 for(unsigned x=0;x<20;x++)if(sprite_mask_coordinate(x,25,20,0,25,20)!=(int)x)mask_mapping_errors++;
 for(unsigned x=0;x<4;x++)if(sprite_mask_coordinate(x,5,4,0,5,4)!=(int)x)mask_mapping_errors++;
 check_pixel("prescaled_mask_identity",mask_mapping_errors,0);
 check_pixel("prescaled_mask_registration_outside",(unsigned)(sprite_mask_coordinate(0,25,20,5,10,8)+1),0);
 check_pixel("prescaled_mask_registration_inside",(unsigned)sprite_mask_coordinate(4,25,20,5,10,8),0);
 uint8_t zero_alpha[16]={0};surface_t alpha_words={.buffer=zero_alpha};
 for(unsigned alpha_plane=0;alpha_plane<2;alpha_plane++)for(unsigned follow=0;follow<2;follow++) {
   word_image.follow_alpha=follow;
   for(unsigned i=0;i<16;i++)((uint16_t*)mask_output.buffer)[i]=0x003f;
   rdpq_attach(&mask_output,NULL);ink_bitmap(&mask_output,&mask_words,alpha_plane?&alpha_words:NULL,&word_image,&mask_sprite,0,0,4,4,9,100,false,NULL);rdpq_detach_wait();
   for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++) {
     uint16_t expected=x%3==0?0x003f:x%3==1?0xf801:0x7821;
     if(follow&&(alpha_plane||!((x+y)&1u)))expected=0x003f;
     if(((uint16_t*)mask_output.buffer)[y*4+x]!=expected)mask_word_errors++;
   }
 }
 check_pixel("mask_words_black_white_gray_explicit_alpha",mask_word_errors,0);
 for(unsigned i=0;i<16;i++){((uint16_t*)mask_words.buffer)[i]=0xffff;((uint16_t*)mask_output.buffer)[i]=0x003f;}
 mask_sprite.value.ink=36;
 rdpq_attach(&mask_output,NULL);ink_bitmap(&mask_output,&mask_words,NULL,&word_image,&mask_sprite,0,0,4,4,36,100,false,NULL);rdpq_detach_wait();
 check_pixel("opaque_word_fast_path_keeps_background_key",((uint16_t*)mask_output.buffer)[0],0x003f);
 surface_free(&mask_words);surface_free(&mask_output);

 free(cache[0].data);memset(&cache[0],0,sizeof(cache[0]));cache_key[0]=cache_bytes=cache_arena_bytes=0;
 surface_free(&full);surface_free(&cropped);surface_free(&reference);surface_free(&optimized);
 // Cropping keeps the virtual authored brush origin, including a16x32
 // texture whose period does not divide the aligned4px crop offset.
 unsigned pattern_bytes=32+16*32*4;
 uint8_t *pattern_data=calloc(1,pattern_bytes);assertf(pattern_data,"pattern fixture");
 memcpy(pattern_data,"FDI2",4);pattern_data[9]=16;pattern_data[11]=32;
 for(unsigned y=0;y<32;y++)for(unsigned x=0;x<16;x++)
   ((uint32_t*)(pattern_data+32))[y*16+x]=(x*16u)<<24|(y*8u)<<16|0x55ff;
 cache[0]=(cached_image_t){.data=pattern_data,.bytes=pattern_bytes,.format=IMAGE_RGBA32};
 strcpy(cache[0].name,"synthetic-pattern.fd2");cache_key[0]=dg_image_key(cache[0].name,0);
 cache_bytes=cache_arena_bytes=pattern_bytes;
 const dg_member_t *pattern_member=&test_members[10];
 n64_film_crop_t pattern_crop=n64_film_crop(director,DG_SPRITES,pattern_member,&pattern_frame,64,64);
 surface_t pattern_full=surface_alloc(FMT_RGBA32,64,64),pattern_part=surface_alloc(FMT_RGBA32,pattern_crop.width,pattern_crop.height);
 rdpq_attach(&pattern_full,NULL);draw_scene_frame(&pattern_full,director,0,0,&pattern_frame,pattern_member,DG_SPRITES);rdpq_detach_wait();
 rdpq_attach(&pattern_part,NULL);draw_scene_frame(&pattern_part,director,-dg_stage_coord(pattern_crop.left),-dg_stage_coord(pattern_crop.top),&pattern_frame,pattern_member,DG_SPRITES);rdpq_detach_wait();
 unsigned pattern_errors=0;
 for(unsigned y=0;y<pattern_crop.height;y++)for(unsigned x=0;x<pattern_crop.width;x++)
   if(((uint32_t*)pattern_part.buffer)[y*pattern_crop.width+x]!=((uint32_t*)pattern_full.buffer)[(y+pattern_crop.top)*64+x+pattern_crop.left])pattern_errors++;
 check_pixel("cropped_pattern16x32_phase",pattern_errors,0);
 check_pixel("cropped_pattern_nonperiod_origin",pattern_crop.left==4&&pattern_crop.top==4,1);
 free(cache[0].data);memset(&cache[0],0,sizeof(cache[0]));cache_key[0]=cache_bytes=cache_arena_bytes=0;
 surface_free(&pattern_full);surface_free(&pattern_part);
 // A stable cached scene plus a small live film must not charge the resident
 // background a second time when testing contiguous temporary allocations.
 const unsigned resident_bytes=32+640*480*4;
 for(unsigned slot=0;slot<3;slot++) {
   unsigned bytes=slot?96:resident_bytes;
   cache[slot]=(cached_image_t){.data=malloc(bytes),.bytes=bytes,.ink=slot?8:0,
       .format=IMAGE_RGBA32,.follow_alpha=true,.last=completed_serial};
   assertf(cache[slot].data,"resident cache fixture allocation");
   strcpy(cache[slot].name,slot==0?"resident-background.fdi":slot==1?"preload-source.fdi":"preload-mask.fdi");
   cache_key[slot]=dg_image_key(cache[slot].name,cache[slot].ink);
   cache_bytes+=bytes;cache_arena_bytes+=bytes;
 }
 sys_get_heap_stats(&before);
 fixture_pressure=malloc(before.free-600*1024);
 void *pressure=fixture_pressure;
 assertf(pressure,"resident cache fixture pressure");
 director->sprites[1]=(dg_sprite_t){.value={.member=DG_ID(1,1,12),.type=16,.width=800,.height=600,.blend=100,.thickness=DG_HAS_BLEND},.visible=true,.stretch=true};
 director->sprites[2]=(dg_sprite_t){.value={.member=DG_ID(1,1,8),.type=16,.width=5,.height=5,.blend=100,.thickness=DG_HAS_BLEND},.visible=true,.stretch=true};
 const uint16_t resident_order[]={1,2};
 void *resident_data=cache[0].data;
 uint64_t opened=image_open_us;
 for(unsigned pass=0;pass<2;pass++) {
   completed_serial=render_serial;render_serial++;
   image_streaming=false;
   image_preload_scene(director,resident_order,0,2,0,0);
   check_pixel(pass?"resident_scene_second_frame_keeps_cache":"resident_scene_first_frame_keeps_cache",
       cache[0].data==resident_data && image_open_us==opened && !values->failed,1);
 }
 free(pressure);fixture_pressure=NULL;
 for(unsigned slot=0;slot<3;slot++) {free(cache[slot].data);memset(&cache[slot],0,sizeof(cache[slot]));cache_key[slot]=0;}
 cache_bytes=cache_arena_bytes=0;

 // Full-frame cache-on/cache-off comparison across dynamic suffix/cursor
 // movement, prefix geometry/colors/palette, background and mutable masks.
 unsigned prefix_bytes=32+256*160*2;
 for(unsigned i=0;i<2;i++) {
   uint8_t *data=calloc(1,prefix_bytes);assertf(data,"prefix asset allocation");
   memcpy(data,"FDI1",4);data[8]=1;data[10]=0;data[11]=160;
   for(unsigned y=0;y<160;y++)for(unsigned x=0;x<256;x++)
     ((uint16_t*)(data+32))[bitmap_tile_index(256,x,y)]=i?(x%7?1:0xffff):0xf801;
   cache[i]=(cached_image_t){.data=data,.bytes=prefix_bytes,.format=IMAGE_RGBA16,.ink=8,.last=completed_serial};
   strcpy(cache[i].name,i?"prefix-mask.fdi":"prefix-source.fdi");cache_key[i]=dg_image_key(cache[i].name,8);
   cache_bytes+=prefix_bytes;cache_arena_bytes+=prefix_bytes;
 }
 memset(director->sprites,0,sizeof(director->sprites));
 director->sprites[1]=(dg_sprite_t){.value={.member=DG_ID(1,1,13),.type=16,.width=320,.height=200,.ink=9,.blend=100,.flags=48,.back_rgb=0xffffff},.visible=true,.stretch=true};
 director->sprites[2]=(dg_sprite_t){.value={.member=DG_ID(1,1,3),.type=16,.x=20,.y=30,.width=4,.height=4,.ink=8,.blend=100,.flags=48,.back_rgb=0xffffff},.visible=true,.stretch=true};
 director->stage_color=0x0000ff;director->transition_serial=transition_consumed=0;transition_active=false;
 surface_t prefix_cached=surface_alloc(FMT_RGBA16,640,480),prefix_plain=surface_alloc(FMT_RGBA16,640,480);
 assertf(prefix_cached.buffer&&prefix_plain.buffer,"prefix framebuffer allocation");
 director->dynamic_count=2;
 director->dynamic_members[0].member=test_members[12];
 director->dynamic_members[1].member=test_members[13];
 unsigned prefix_errors=0;
 for(unsigned pass=0;pass<10;pass++) {
   if(pass==1)director->sprites[2].value.x=40;
   if(pass==3)director->sprites[1].value.fore_rgb=0x000040;
   if(pass==4){director->palette_active=true;director->active_palette[7]^=0x101010;}
   if(pass==5)director->sprites[1].value.x=5;
   if(pass==6)director->stage_color=0x4000ff;
   if(pass==7)director->dynamic_members[0].member.reg_x=1;
   if(pass==8)director->dynamic_members[1].member.reg_x=2;
   if(pass==9)director->dynamic_members[0].member.width=319;
   unsigned cursor_x=pass<2?500:550;
   prefix_draw(&prefix_cached,true,cursor_x);prefix_draw(&prefix_plain,false,cursor_x);
   if(memcmp(prefix_cached.buffer,prefix_plain.buffer,640*480*2))prefix_errors++;
 }
 check_pixel("static_prefix_exact_dynamic_cursor_invalidations",prefix_errors,0);
 check_pixel("static_prefix_two_hits",scene_prefix_hits,2);
 check_pixel("static_prefix_eight_misses",scene_prefix_misses,8);
 lv_t white_mask=lv_call(values,&frame,"image",3,(lv_t[]){lv_num(320),lv_num(200),lv_num(32)});
 lv_call(values,&frame,"fill",3,(lv_t[]){white_mask,lv_rect(values,0,0,320,200),lv_make(LV_COLOR,0xffffff)});
 lv_set(values,NULL,"image",lv_make(LV_MEMBER,DG_ID(1,1,14)),white_mask);
 prefix_draw(&prefix_cached,true,550);prefix_draw(&prefix_plain,false,550);
 check_pixel("static_prefix_mutable_mask_invalidates",!scene_prefix_valid&&!memcmp(prefix_cached.buffer,prefix_plain.buffer,640*480*2),1);
 dg_images_dispose(director);director->sprites[2].visible=false;
 director->dynamic_members[0].member=(dg_member_t){.id=DG_ID(1,1,13),.number=13,.cast=1,.type=8,.width=320,.height=200,.shape=1,.filled=true,.pattern=1};
 prefix_draw(&prefix_cached,true,550);prefix_draw(&prefix_plain,false,550);
 check_pixel("static_prefix_plain_shape_cached",scene_prefix_valid&&!memcmp(prefix_cached.buffer,prefix_plain.buffer,640*480*2),1);
 lv_set(values,NULL,"pattern",lv_make(LV_MEMBER,DG_ID(1,1,13)),lv_num(2));
 prefix_draw(&prefix_cached,true,550);prefix_draw(&prefix_plain,false,550);
 check_pixel("static_prefix_live_pattern_invalidates",dg_member_pattern(director,&director->dynamic_members[0].member)==2&&!scene_prefix_valid&&!memcmp(prefix_cached.buffer,prefix_plain.buffer,640*480*2),1);
 // Reclaimable cache storage cannot turn a legal live-film allocation into
 // a capacity failure. A synthetic retained prefix is safe to discard.
 scene_prefix_plane=surface_alloc(FMT_RGBA16,640,480);assertf(scene_prefix_plane.buffer,"prefix reclaim fixture");
 scene_prefix_valid=true;
 sys_get_heap_stats(&before);fixture_pressure=malloc(before.free-128*1024);pressure=fixture_pressure;assertf(pressure,"prefix pressure fixture");
 sys_get_heap_stats(&after);check_pixel("prefix_pressure_is_real",after.free<256*1024,1);
 surface_t pressure_surface=image_temporary_surface(FMT_RGBA32,384,384);
 debugf("DIRECTOR64 PREFIX_PRESSURE surface=%p cache=%p valid=%u\n",pressure_surface.buffer,scene_prefix_plane.buffer,scene_prefix_valid);
 check_pixel("static_prefix_reclaims_under_pressure",pressure_surface.buffer&&!scene_prefix_plane.buffer&&!scene_prefix_valid,1);
 if(pressure_surface.buffer)surface_free(&pressure_surface);
 free(pressure);fixture_pressure=NULL;
 surface_free(&prefix_cached);surface_free(&prefix_plain);scene_prefix_release();
 check_pixel("runtime_failure",values->failed,0);
 debugf("DIRECTOR64 FILM_DONE failed=%u\n",failed);while(1){}
}
