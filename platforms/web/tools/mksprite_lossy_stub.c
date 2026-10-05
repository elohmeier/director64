// The browser build of mksprite packs font atlases only (CI4, I4, IA8); the
// lossy H.264 and BC1 codecs, and the x264 library behind them, are left out.
#include <stdio.h>
#include "mksprite.h"

int mksprite_convert_lossy(const char *infn, const char *outfn, const parms_t *pm, int compress) {
  (void)outfn, (void)pm, (void)compress;
  fprintf(stderr, "%s: lossy sprite formats are not in this build\n", infn);
  return 1;
}
int mksprite_convert_bc1(const char *infn, const char *outfn, const parms_t *pm, int compress) {
  (void)outfn, (void)pm, (void)compress;
  fprintf(stderr, "%s: BC1 sprites are not in this build\n", infn);
  return 1;
}
