#include "archive.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char flash[2*ARCHIVE_BYTES];
static unsigned cut=ARCHIVE_BYTES;
static archive_t archive, reload;
static bool read_flash(void *ctx,size_t offset,void *data,size_t bytes) {
  (void)ctx;assert(offset+bytes<=sizeof(flash));memcpy(data,flash+offset,bytes);return true;
}
static bool write_flash(void *ctx,size_t offset,const void *data,size_t bytes) {
  (void)ctx;assert(bytes==ARCHIVE_BYTES);memcpy(flash+offset,data,cut);return cut==bytes;
}
int main(void) {
  const save_backend_t backend={read_flash,write_flash,NULL};
  const char original[]={0,1,2,0,(char)255,4};
  char result[16];unsigned length;
  memset(flash,255,sizeof(flash));assert(archive_load(&archive,backend)==SAVE_BLANK);
  assert(archive_write(&archive,"ByggFil.txt",original,sizeof(original)));
  assert(archive_load(&reload,backend)==SAVE_VALID && reload.generation==1);
  assert(archive_read(&reload,"BYGGFIL.TXT",result,sizeof(result),&length));
  assert(length==sizeof(original) && !memcmp(result,original,length));
  static const unsigned cuts[]={0,1,127,128,32768,65535};
  for(unsigned i=0;i<sizeof(cuts)/sizeof(*cuts);i++) {
    memset(flash+ARCHIVE_BYTES,255,ARCHIVE_BYTES);cut=cuts[i];
    assert(!archive_write(&archive,"ByggFil.txt","replacement",11));
    assert(archive.generation==1 && archive_load(&reload,backend)==SAVE_VALID);
    assert(reload.generation==1 && archive_read(&reload,"ByggFil.txt",result,sizeof(result),&length));
    assert(length==sizeof(original) && !memcmp(result,original,length));
  }
  cut=ARCHIVE_BYTES;assert(archive_write(&archive,"ByggFil.txt","replacement",11));
  assert(archive_load(&reload,backend)==SAVE_VALID && reload.generation==2);
  assert(!archive_write(&archive,"other-game.dat","x",1));
  memset(flash,27,sizeof(flash));assert(archive_load(&reload,backend)==SAVE_CORRUPT);
  assert(!archive_write(&reload,"ByggFil.txt","x",1));
  puts("Christmas FlashRAM: binary data, torn-write recovery and corrupt-save protection passed");
}
