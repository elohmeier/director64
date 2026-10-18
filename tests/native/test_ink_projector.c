// Independent Birthday projector observations, 2026-10-18, under Wine8:
// PF3.EXE 9.0r383 / PROJ.DLL 9.0r371, executing authored850 movies with
// DIRAPI.DLL and IML32.DLL 8.5.1r104 (verified loaded modules).
// All inputs are synthetic one-pixel 32-bit image colors.
// These expected values deliberately do not use the renderer to generate them.
#include "ink.h"
#include "projector_ink_observations.h"
#include <assert.h>
#include <stdio.h>
static const unsigned source[] = {0,64,128,192,255};
static const unsigned foreground[] = {0,50,128,255};
static const unsigned background[] = {0,64,128,255};
static const unsigned char observed[2][5][4][4] = {
  { // ink40
    {{255,192,128,0}, {255,242,178,50}, {255,255,255,128}, {255,255,255,255}},
    {{255,208,160,64}, {255,255,210,114}, {255,255,255,192}, {255,255,255,255}},
    {{255,224,192,128}, {255,255,242,178}, {255,255,255,255}, {255,255,255,255}},
    {{255,240,224,192}, {255,255,255,242}, {255,255,255,255}, {255,255,255,255}},
    {{255,255,255,255}, {255,255,255,255}, {255,255,255,255}, {255,255,255,255}},
  },
  { // ink41
    {{0,0,0,0}, {50,50,50,50}, {128,128,128,128}, {255,255,255,255}},
    {{0,16,32,64}, {50,66,82,114}, {128,144,160,192}, {255,255,255,255}},
    {{0,32,64,128}, {50,82,114,178}, {128,160,192,255}, {255,255,255,255}},
    {{0,48,96,192}, {50,98,146,242}, {128,176,224,255}, {255,255,255,255}},
    {{0,63,127,255}, {50,113,177,255}, {128,191,255,255}, {255,255,255,255}},
  },
};
int main(void) {
  for(unsigned ink=0;ink<2;ink++)for(unsigned s=0;s<5;s++)
    for(unsigned f=0;f<4;f++)for(unsigned b=0;b<4;b++) {
      unsigned actual=ink?dg_ink_tint_channel(source[s],foreground[f],background[b])
                         :dg_ink_lighten_channel(source[s],foreground[f],background[b]);
      if(actual!=observed[ink][s][f][b])
        fprintf(stderr,"ink%u source%u fore%u back%u: %u != %u\n",ink+40,source[s],foreground[f],background[b],actual,observed[ink][s][f][b]);
      assert(actual==observed[ink][s][f][b]);
    }
  for (unsigned i = 0; i < sizeof(projector_precision) / sizeof(*projector_precision); i++) {
    const unsigned short *p = projector_precision[i];
    unsigned actual = p[0] == 40 ? dg_ink_lighten_channel(p[1], p[2], p[3])
                                 : dg_ink_tint_channel(p[1], p[2], p[3]);
    assert(actual == p[4]);
  }
  // Stage and copyPixels have distinct blend scales. These compare the
  // independently observed stage result, including percent rounding.
  for (unsigned i = 0; i < sizeof(projector_stage_blend) / sizeof(*projector_stage_blend); i++) {
    const unsigned *p = projector_stage_blend[i];
    const unsigned src[] = {20, 90, 180}, fore[] = {20, 40, 60}, back[] = {220, 180, 140};
    for (unsigned c = 0; c < 3; c++) {
      unsigned value = p[1] < 100
          ? dg_sprite_blend_channel(p[0] == 40 || p[0] == 41 ? src[c]
              : dg_ink_colorize_channel(src[c], fore[c], back[c]), c == 2 ? 255 : 0, p[1])
          : p[0] == 40 ? dg_ink_lighten_channel(src[c], fore[c], back[c])
          : p[0] == 41 ? dg_ink_tint_channel(src[c], fore[c], back[c])
          : dg_ink_arithmetic(p[0]) ? dg_ink_channel(src[c], c == 2 ? 255 : 0, p[0])
          : dg_ink_colorize_channel(src[c], fore[c], back[c]);
      assert(value == ((p[2] >> (c * 8)) & 255));
    }
  }
  assert(dg_ink_channel(60,100,35)==40);
  puts("original Birthday 8.5.1r104 engine: Lighten/Darken and SubtractPin passed");
}
