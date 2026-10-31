#include "cursor_resource.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  wc_resource_cursor_t cursor={0};
  uint8_t bytes[]={'D','6','4','C',0,4,0,1,0,1,0,0,0,1,0,0,0,1,2,3};
  assert(wc_resource_cursor_decode(&cursor,2,bytes,sizeof(bytes)));
  assert(cursor.width==4 && cursor.height==1 && cursor.hot_x==1 && !cursor.hot_y);
  uint32_t background=UINT32_C(0xff123456);
  assert(wc_resource_cursor_pixel(cursor.pixels[0],background)==UINT32_C(0xff000000));
  assert(wc_resource_cursor_pixel(cursor.pixels[1],background)==UINT32_MAX);
  assert(wc_resource_cursor_pixel(cursor.pixels[2],background)==background);
  assert(wc_resource_cursor_pixel(cursor.pixels[3],background)==UINT32_C(0xffedcba9));
  for(size_t n=0;n<sizeof(bytes);n++) assert(!wc_resource_cursor_decode(&cursor,2,bytes,n));
  bytes[16]=4;assert(!wc_resource_cursor_decode(&cursor,2,bytes,sizeof(bytes)));
  bytes[16]=0;bytes[9]=4;assert(!wc_resource_cursor_decode(&cursor,2,bytes,sizeof(bytes)));
  puts("source cursor masks: black/white/transparent/invert and canvas/hotspot bounds pass");
}
