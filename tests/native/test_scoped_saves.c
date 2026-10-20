#include "game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char image[131072];
static bool read_image(void *c,size_t at,void *out,size_t n) {(void)c;memcpy(out,image+at,n);return true;}
static bool write_image(void *c,size_t at,const void *in,size_t n) {(void)c;memcpy(image+at,in,n);return true;}
int main(void) {
  save_backend_t backend={read_image,write_image,NULL};
  memset(image,255,sizeof(image));assert(game_save_load(backend)==SAVE_BLANK);
  assert(!game_directory(NULL,"Z:\\setup",false));
  assert(game_directory(NULL,"D:\\setup",false));
  assert(!game_directory(NULL,"C:\\Spieler",false));
  assert(game_directory(NULL,"C:\\Spieler\\",true));
  assert(!*game_nth_file(NULL,"C:\\Spieler\\",1));
  assert(game_directory(NULL,"C:\\Spieler\\ANNA",true));
  assert(!strcmp(game_nth_file(NULL,"C:\\Spieler",1),"anna\\"));
  assert(game_write_file("C:\\Spieler\\Anna\\settings.ini","one",3));
  assert(game_write_file("C:/Spieler/Ben/settings.ini","two",3));
  assert(!game_write_file("C:/Spieler/../escape.ini","no",2));
  assert(!game_write_file("C:/Spieler/Ben/other.exe","no",2));
  assert(game_save_load(backend)==SAVE_VALID);
  char data[16];unsigned n=0;
  assert(game_read_file("C:/SPIELER/ANNA/settings.ini",data,sizeof(data),&n)&&!strcmp(data,"one"));
  assert(game_read_file("C:/SPIELER/BEN/settings.ini",data,sizeof(data),&n)&&!strcmp(data,"two"));
  assert(!strcmp(game_nth_file(NULL,"C:\\Spieler",1),"anna\\"));
  assert(!strcmp(game_nth_file(NULL,"C:\\Spieler",2),"ben\\"));
  assert(!*game_nth_file(NULL,"C:\\Spieler",3));
  assert(!strcmp(game_nth_file(NULL,"C:\\Spieler\\Ben",1),"settings.ini"));
  puts("scoped saves: empty folders, distinct profiles, listings, traversal and reload passed");
}
