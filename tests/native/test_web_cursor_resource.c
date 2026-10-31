#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static wc_t compositor;
static dg_runtime_t director;
static lv_runtime_t values;
static uint8_t asset[]={'D','6','4','C',0,4,0,1,0,1,0,0,0,1,0,0,0,1,2,3};
static bool missing;
static unsigned reads;
static uint32_t read_asset(void *ctx,const char *name,uint8_t *out,uint32_t cap) {
  (void)ctx;assert(!strcmp(name,"d64-cursor-2.dcur"));reads++;
  if(missing)return 0;
  if(out){assert(cap>=sizeof(asset));memcpy(out,asset,sizeof(asset));}
  return sizeof(asset);
}
int main(void) {
  static const lv_movie_t code={.name="SOURCE-CURSOR.DXR"};
  static const dg_frame_t frame={0};
  static const dg_movie_t movie={.id=1,.code=&code,.frame_count=1,.frames=&frame};
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&director,&movie,1,NULL,0));
  director.stage_color=0x123456;
  director.cursor=(dg_cursor_t){.resource=2};
  assert(wc_native_cursor(&director)==-1); // Source mask/invert must stay software.
  wc_init(&compositor,(wc_host_t){.read=read_asset},4096);
  assert(wc_render(&compositor,&director,10,10,true,0));
  assert(!values.failed);
  const wc_resource_cursor_t *metadata=wc_source_cursor(&compositor,&values,2);
  assert(metadata && metadata->hot_x==1 && metadata->width==4);
  assert(wc_resource_cursor_hash(metadata)!=0);
  unsigned at=10*WC_WIDTH+9;
  uint32_t background=compositor.scene[at];
  assert(background!=UINT32_MAX && background!=UINT32_C(0xff000000));
  assert(compositor.pixels[at]==UINT32_C(0xff000000));
  assert(compositor.pixels[at+1]==UINT32_MAX);
  assert(compositor.pixels[at+2]==background);
  assert(compositor.pixels[at+3]==(background^UINT32_C(0x00ffffff)));
  unsigned loaded=reads;
  assert(wc_render(&compositor,&director,20,10,true,0));
  assert(reads==loaded && compositor.pixels[at]==background);
  assert(wc_render(&compositor,&director,0,0,true,0));
  assert(!values.failed); // Hotspot clipping retains correct source indexing.
  wc_release(&compositor);
  missing=true;wc_init(&compositor,(wc_host_t){.read=read_asset},4096);
  wc_render(&compositor,&director,10,10,true,0);
  assert(values.failed && strstr(values.error,"executable cursor resource"));
  wc_release(&compositor);
  puts("source cursor compositor: exact masks, full canvas, hotspot clipping, cache and missing-asset failure pass");
}
