// Recreate the native host in one process, releasing owned film/image state
// before host callbacks or package tables become unavailable.
#include "web_runtime.h"
#include "package.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static dg_runtime_t director;
static lv_runtime_t values;
static wc_t compositor;
static web_host_t host;
static bool booted,host_alive;
static dg_package_t package;
static const dg_movie_t *const *dg_registry;
static const char *const *global_names;
static unsigned dg_registry_count,global_count,closed;
static char package_error[256];
#if DG_D5 || DG_D10
static void close_videos(void) {assert(host_alive&&package.bytes);closed++;}
#endif
#if DG_D10
static void close_flash_audio(void) {assert(host_alive&&package.bytes);closed++;}
#endif
#include "../../platforms/web/lifecycle.inc"
static const lv_movie_t code={.name="RECREATE.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t frame={0};
static const dg_member_t member={.id=DG_ID(1,1,1),.cast=1,.number=1,.type=2,.film_count=1};
static const dg_movie_t movie={.id=1,.code=&code,.cast_count=1,.casts=&cast,
 .member_count=1,.members=&member,.frame_count=1,.frames=&frame};
static void sound(void *ctx,unsigned channel,const dg_member_t *m) {
  (void)channel;(void)m;
  assert(ctx==&host_alive&&host_alive&&package.bytes);
}
int main(void) {
  for(unsigned i=0;i<16;i++) {
    assert(!booted&&!dg_registry&&!package.bytes&&!host.ctx);
    host_alive=true;host.ctx=&host_alive;
    package.bytes=malloc(16);assert(package.bytes);
    package.blocks=calloc(1,sizeof(void *));assert(package.blocks);
    package.blocks[0]=calloc(1,sizeof(dg_movie_t *));assert(package.blocks[0]);
    package.block_count=package.block_capacity=1;
    package.registry=package.blocks[0];dg_registry=package.registry;
    dg_registry_count=1;global_count=0;
    dg_init(&director,&values,(dg_platform_t){.sound=sound},&host_alive,NULL,0,1);
    assert(dg_enter(&director,&movie,1,NULL,0));
    director.sounds[0]=member.id;
    wc_init(&compositor,(wc_host_t){0},1024);
    booted=true;
#if DG_D10
    director.sprites[1]=(dg_sprite_t){.value={.member=member.id,.type=16},.visible=true};
    dg_sprite_changed(&director,1);dg_films_present(&director,true);
    lv_frame_t caller={.movie=&code};
    lv_t image=lv_call(&values,&caller,"image",3,(lv_t[]){lv_num(128),lv_num(128),lv_num(32)});
    assert(image&&!values.failed&&director.film_playback&&director.imaging);
#endif
#if DG_D5
    director.suspended_stage=calloc(DG_SPRITES,sizeof(dg_sprite_t));assert(director.suspended_stage);
#endif
    d64_shutdown();
    host_alive=false;
    assert(!booted&&!package.bytes&&!package.blocks&&!dg_registry&&!global_names);
    assert(!dg_registry_count&&!global_count);
    assert(!director.movie&&!director.values&&!host.ctx&&!values.external_roots);
#if DG_D10
    assert(!director.film_playback&&!director.imaging&&!director.linked_children&&!director.flash_state);
#endif
    unsigned calls=closed;d64_shutdown();assert(closed==calls);
  }
  puts("host lifecycle: repeated teardown/recreation, owned resources and callback ordering passed");
}
