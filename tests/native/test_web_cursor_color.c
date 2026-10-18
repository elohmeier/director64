#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static wc_t compositor;
static lv_runtime_t values;
static uint8_t asset[1056]={'F','D','I','2',0,0,4,32,0,16,0,16,0,0,0,0,1,1,0,32};
static uint32_t read_asset(void *ctx,const char *name,uint8_t *out,uint32_t size) {
  (void)ctx;(void)name;
  if(out){assert(size>=sizeof(asset));memcpy(out,asset,sizeof(asset));}
  return sizeof(asset);
}
int main(void) {
  const dg_member_t member={.type=1,.source_xtra=5,.width=16,.height=16,
    .reg_x=7,.asset="source-free-cursor.fd2"};
  memcpy(asset+32,(uint8_t[]){0,255,0,255,0,0,0,255,255,255,255,0,255,0,0,255},16);
  wc_init(&compositor,(wc_host_t){.read=read_asset},4096);
  dg_cursor_bitmap_t cursor;
  assert(wc_cursor_bitmap(&compositor,&values,&member,NULL,&cursor));
  assert(cursor.color&&cursor.hot_x==7&&cursor.hot_y==0);
  uint16_t pixels[256];dg_cursor_pixels_ink(&cursor,pixels,dg_cursor_ink(0,0,255));
  assert(pixels[0]==dg_cursor_ink(0,255,0));
  assert(pixels[1]==1&&!(pixels[2]&1)&&pixels[3]==dg_cursor_ink(255,0,0));
  assert(!values.failed);wc_release(&compositor);
  puts("web color cursor: authored colors, alpha and registration survive asset loading");
}
