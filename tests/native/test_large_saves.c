// Painting-sized binary files, atomic replacement, and legacy preference
// migration through the same storage implementation used by both hosts.
#include "game.h"
#include "game_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t image[2u * DIRECTOR64_SAVE_SLOT_BYTES];
static bool fail_write;
static bool read_image(void *ctx, size_t at, void *out, size_t n) {
  (void)ctx;
  if (at > sizeof(image) || n > sizeof(image) - at) return false;
  memcpy(out, image + at, n); return true;
}
static bool write_image(void *ctx, size_t at, const void *data, size_t n) {
  (void)ctx;
  if (at > sizeof(image) || n > sizeof(image) - at) return false;
  memcpy(image + at, data, fail_write ? 16 : n);
  return !fail_write;
}
static const save_backend_t backend = {read_image, write_image, NULL};
static void put32(uint8_t *p, uint32_t n) {
  p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;
}
static uint32_t checksum(const uint8_t *p, size_t n) {
  uint32_t c=UINT32_MAX;
  for(size_t i=0;i<n;i++) {
    c ^= i>=16 && i<20 ? 0 : p[i];
    for(unsigned j=0;j<8;j++) c=(c>>1)^(0xedb88320u&(0u-(c&1u)));
  }
  return c^UINT32_MAX;
}
static void legacy_generation(uint8_t *p, unsigned generation, uint8_t value) {
  memset(p,0,65536);memcpy(p,"D64S",4);put32(p+4,1);put32(p+8,generation);
  const char *name="settings.usr";size_t n=strlen(name);
  put32(p+12,(uint32_t)(1+n+4+3));put32(p+20,1);
  p[32]=(uint8_t)n;memcpy(p+33,name,n);put32(p+33+n,3);
  p[37+n]=1;p[38+n]=0;p[39+n]=value;put32(p+16,checksum(p,65536));
}
int main(void) {
  uint8_t legacy[131072], original[131072];
  legacy_generation(legacy,5,2);legacy_generation(legacy+65536,6,3);
  memcpy(original,legacy,sizeof(legacy));memset(image,0xA7,sizeof(image));
  assert(game_save_import(image,sizeof(image),legacy,sizeof(legacy)));
  assert(!memcmp(legacy,original,sizeof(legacy)));
  assert(game_save_load(backend)==SAVE_VALID && game_save_generation()==6);
  unsigned length=0;char prefs[16];
  assert(game_read_file("settings.usr",prefs,sizeof(prefs),&length) && length==3);
  assert(prefs[0]==1 && prefs[1]==0 && prefs[2]==3);
  assert(game_file_exists("settings.usr") && !game_file_exists("images"));
  size_t size=1024u*1024u;char *data=malloc(size),*readback=malloc(size+1);
  assert(data && readback);
  for(size_t i=0;i<size;i++) data[i]=(char)(i*73u);
  assert(game_write_file("images","old",3));
  assert(game_write_file("temp.cst",data,(unsigned)size));
  assert(game_file_exists("TEMP") && game_file_exists("images.cst"));
  fail_write=true;
  assert(!game_rename_file("temp","images"));
  fail_write=false;
  assert(game_save_load(backend)==SAVE_VALID);
  assert(game_read_file("images",prefs,sizeof(prefs),&length) && !strcmp(prefs,"old"));
  assert(game_file_exists("temp"));
  assert(game_rename_file("temp","images"));
  assert(!game_file_exists("temp.cst") && game_file_exists("images"));
  assert(game_save_load(backend)==SAVE_VALID);
  assert(game_read_file("images",readback,(unsigned)size+1,&length));
  assert(length==size && !memcmp(data,readback,size));
  assert(!game_rename_file("images","other.exe"));
  assert(!game_rename_file("missing.usr","settings.usr"));
  assert(game_remove_file("missing.usr"));
  fail_write=true;assert(!game_remove_file("images"));fail_write=false;
  assert(game_save_load(backend)==SAVE_VALID && game_file_exists("images"));
  assert(game_remove_file("images.cst") && !game_file_exists("images"));
  assert(game_file_exists("settings.usr"));
  legacy[65536+40]^=1;
  assert(game_save_import(image,sizeof(image),legacy,sizeof(legacy)));
  assert(game_save_load(backend)==SAVE_VALID && game_save_generation()==5);
  assert(game_read_file("settings.usr",prefs,sizeof(prefs),&length) && prefs[2]==2);
  legacy[65536+40]^=1;
  legacy[40]^=1;legacy[65536+40]^=1;
  memset(image,0xA7,sizeof(image));
  assert(!game_save_import(image,sizeof(image),legacy,sizeof(legacy)));
  for(size_t i=0;i<sizeof(image);i++) assert(image[i]==0xA7);
  memset(legacy,255,sizeof(legacy));
  assert(game_save_import(image,sizeof(image),legacy,sizeof(legacy)));
  assert(game_save_load(backend)==SAVE_BLANK);
  assert(!game_save_import(image,sizeof(image),legacy,sizeof(legacy)-1));
  legacy_generation(legacy,UINT32_MAX,9);
  assert(game_save_import(image,sizeof(image),legacy,sizeof(legacy)));
  assert(game_save_load(backend)==SAVE_VALID && game_save_generation()==UINT32_MAX);
  assert(!game_write_file("settings.usr","new",3));
  assert(!game_remove_file("settings.usr"));
  assert(game_file_exists("settings.usr"));
  assert(game_read_file("settings.usr",prefs,sizeof(prefs),&length) && prefs[2]==9);
  free(readback);free(data);
  puts("large saves: 16 MiB slots, legacy binary preferences, atomic rename/delete and failures pass");
}
