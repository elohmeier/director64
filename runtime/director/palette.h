#ifndef DIRECTOR64_PALETTE_H
#define DIRECTOR64_PALETTE_H
#include <stdbool.h>
#include <stdint.h>

// Director's system palettes. Numeric color definitions are format facts;
// the construction also lives in the converter's bitmap module.
static inline void dg_system_palette(uint32_t out[256], bool windows) {
  unsigned n=0;
  const unsigned steps[6]={255,204,153,102,51,0};
  if (!windows) {
    for(unsigned r=0;r<6;r++) for(unsigned g=0;g<6;g++) for(unsigned b=0;b<6;b++)
      if(r!=5 || g!=5 || b!=5) out[n++]=(steps[r]<<16)|(steps[g]<<8)|steps[b];
    const unsigned shades[10]={238,221,187,170,136,119,85,68,34,17};
    for(unsigned c=0;c<4;c++) for(unsigned j=0;j<10;j++)
      out[n++]=((c==0||c==3)?shades[j]<<16:0)|((c==1||c==3)?shades[j]<<8:0)|((c==2||c==3)?shades[j]:0);
    out[n]=0;
    return;
  }
  const uint32_t prefix[20]={0xffffff,0x00ffff,0xff00ff,0x0000ff,0xffff00,0x00ff00,0xff0000,0x808080,0xa0a0a4,0xfffbf0,
    0x333333,0x996600,0x336633,0x003399,0xcc00ff,0x880000,0xffcc66,0xff99cc,0xdddddd,0xff9900};
  for(unsigned i=0;i<20;i++)out[n++]=prefix[i];
  for(unsigned g=2;g<4;g++)for(unsigned b=0;b<6;b++)out[n++]=0xff0000|(steps[g+1]<<8)|steps[b];
  for(unsigned b=1;b<5;b++)out[n++]=0xff0000|steps[b];
  for(unsigned r=1;r<5;r++)for(unsigned g=0;g<6;g++)for(unsigned b=0;b<6;b++)out[n++]=(steps[r]<<16)|(steps[g]<<8)|steps[b];
  for(unsigned b=1;b<5;b++)out[n++]=0x00ff00|steps[b];
  for(unsigned g=1;g<5;g++)for(unsigned b=0;b<6;b++)out[n++]=(steps[g]<<8)|steps[b];
  for(unsigned b=1;b<5;b++)out[n++]=steps[b];
  const unsigned shades[9]={238,221,170,136,119,85,68,34,17};
  for(int c=16;c>=0;c-=8)for(unsigned j=0;j<9;j++)out[n++]=shades[j]<<(unsigned)c;
  const uint32_t suffix[17]={0x222230,0xff9999,0xffccff,0x99d4ff,0x99d499,0xffff99,0xf0f0f0,0xa6caf0,0xc0dcc0,0xc0c0c0,
    0x008080,0x800080,0x000080,0x808000,0x008000,0x800000,0};
  for(unsigned i=0;i<17;i++)out[n++]=suffix[i];
  out[66]=0xd408ff;out[95]=0xa16600;out[166]=0x336e33;out[172]=0x33333b;out[204]=0x0033a1;out[215]=0x900000;
}
#endif
