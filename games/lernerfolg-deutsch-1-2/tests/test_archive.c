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
  const char players[]="[#count: 2]\r", profile[]={0,1,2,0,(char)255,4};
  char result[32];unsigned length;
  memset(flash,255,sizeof(flash));assert(archive_load(&archive,backend)==SAVE_BLANK);
  assert(archive_file_id("PLAYERS.INI")==0 && archive_file_id("time_reg.txt")==5);
  assert(archive_file_id("save")==6 && archive_file_id("SAVE01")==7 && archive_file_id("save08")==14);
  assert(archive_file_id("save09")<0 && archive_file_id("save0")<0 && archive_file_id("deutsch.ini")<0);
  assert(archive_write(&archive,"Players.ini",players,sizeof(players)-1));
  assert(archive_write(&archive,"save03",profile,sizeof(profile)));
  assert(archive_load(&reload,backend)==SAVE_VALID && reload.generation==2);
  assert(archive_read(&reload,"players.INI",result,sizeof(result),&length));
  assert(length==sizeof(players)-1 && !memcmp(result,players,length));
  assert(archive_read(&reload,"save03",result,sizeof(result),&length));
  assert(length==sizeof(profile) && !memcmp(result,profile,length));
  // Rewriting an earlier file shifts the later profile without corrupting it.
  assert(archive_write(&archive,"players.ini","[#count: 3, #renamed: 1]\r",25));
  assert(archive_load(&reload,backend)==SAVE_VALID);
  assert(archive_read(&reload,"save03",result,sizeof(result),&length));
  assert(length==sizeof(profile) && !memcmp(result,profile,length));
  static const unsigned cuts[]={0,1,127,128,32768,65535};
  for(unsigned i=0;i<sizeof(cuts)/sizeof(*cuts);i++) {
    unsigned slot=archive.active_slot==0?1:0;
    memset(flash+slot*ARCHIVE_BYTES,255,ARCHIVE_BYTES);cut=cuts[i];
    assert(!archive_write(&archive,"save03","replacement",11));
    assert(archive_load(&reload,backend)==SAVE_VALID);
    assert(archive_read(&reload,"save03",result,sizeof(result),&length));
    assert(length==sizeof(profile) && !memcmp(result,profile,length));
    cut=ARCHIVE_BYTES;
  }
  memset(flash,0,sizeof(flash));
  assert(archive_load(&reload,backend)==SAVE_BLANK);
  flash[9]=1;
  assert(archive_load(&reload,backend)==SAVE_CORRUPT);
  puts("Deutsch FlashRAM: named preferences, save-cast files, torn-write recovery and corrupt-save protection passed");
}
