// Coverage-guided binary tests use synthetic seeds only, never retail media.
#include "compositor.h"
#include "package.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t length) {
  if (!length || length > 1024u * 1024u) return 0;
  if (*data++ & 1) {
    static const uint8_t abi[DG_PACKAGE_ABI_BYTES]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    dg_package_t package={0}; char error[256];
    dg_package_load(&package,data,length-1,abi,error,sizeof(error));
    dg_package_free(&package);
  } else if (--length >= 32) {
    unsigned width=(unsigned)data[8]*256+data[9],height=(unsigned)data[10]*256+data[11];
    if (!width || !height || width>256 || height>256) return 0;
    uint32_t *pixels=malloc((size_t)width*height*sizeof(*pixels));
    if (!pixels) abort();
    bool follow=false;
    wc_decode(data,(uint32_t)length,width,height,data[12]%42,pixels,&follow);
    free(pixels);
  }
  return 0;
}
