#include "image_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  uint8_t data[32]={0};
  memcpy(data,"FDI1",4);
  data[8]=3; data[9]=32; data[10]=2; data[11]=88; // 800 x 600
  data[16]=1; data[27]=0xa2;
  data[28]=2; data[29]=3; data[30]=200; data[31]=150;
  unsigned l,t,r,b;
  assert(dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  assert(l==8 && t==12 && r==800 && b==600);
  // A source without FollowAlpha paints the full Copy rectangle.
  assert(!dg_image_coverage_hint(data,32,0,false,&l,&t,&r,&b));
  assert(dg_image_coverage_hint(data,32,8,false,&l,&t,&r,&b));
  // Rich text retains its authored background except under transparent ink.
  data[16]|=2;
  assert(!dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  assert(dg_image_coverage_hint(data,32,36,true,&l,&t,&r,&b));
  data[16]=1;
  // Quantization rounds outwards, including a partial final block.
  data[9]=31; data[11]=87;
  assert(dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  assert(r==799 && b==599);
  data[28]=data[29]=data[30]=data[31]=0;
  assert(dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  assert(!l && !t && !r && !b);
  // Palette brushes acquire coverage from their live palette.
  memcpy(data,"FDIC",4); data[16]=5;
  assert(!dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  memcpy(data,"FDI1",4); data[16]=1;
  data[27]=0; // legacy images have no hint
  assert(!dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  data[27]=0xa2; data[28]=data[29]=1;
  assert(!dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  data[30]=data[31]=255; // outside the stored virtual extent
  assert(!dg_image_coverage_hint(data,32,8,true,&l,&t,&r,&b));
  assert(!dg_image_coverage_hint(data,31,8,true,&l,&t,&r,&b));
  puts("precomputed image coverage: conservative bounds, legacy fallback and ink/palette semantics passed");
  return 0;
}
