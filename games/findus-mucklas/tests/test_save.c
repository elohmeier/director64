#include "archive.h"
#include "game.h"
#include <assert.h>
#include <string.h>
static unsigned char flash[131072];
static bool fail_write;
static bool read_flash(void *ctx,size_t off,void *data,size_t len){(void)ctx;if(off>sizeof(flash)||len>sizeof(flash)-off)return false;memcpy(data,flash+off,len);return true;}
static bool write_flash(void *ctx,size_t off,const void *data,size_t len){(void)ctx;if(fail_write||off>sizeof(flash)||len>sizeof(flash)-off)return false;memcpy(flash+off,data,len);return true;}
int main(void){
 memset(flash,255,sizeof(flash));save_backend_t b={read_flash,write_flash,0};char text[1024];unsigned n;
 assert(game_save_load(b)==SAVE_BLANK);
 assert(!game_read_file("player1.p3",text,sizeof(text),&n));
 assert(game_read_file("pettson3.ini",text,sizeof(text),&n));assert(strstr(text,"cdrom=d"));
 assert(strstr(text,"\rVind=0\r")&&!strstr(text,"Vind=1"));
 assert(!game_write_file("../player1.p3","x",1));
 assert(game_write_file("PLAYER1.P3","first",5));assert(game_save_generation()==1);
 fail_write=true;assert(!game_write_file("player1.p3","lost",4));fail_write=false;
 assert(game_save_load(b)==SAVE_VALID);assert(game_read_file("player1.p3",text,sizeof(text),&n));assert(n==5&&!strcmp(text,"first"));
 assert(game_write_file("moblemang.dxt","furniture",9));assert(game_write_file("player6.p3","sixth",5));
 assert(game_save_load(b)==SAVE_VALID);assert(game_read_file("MOBLEMAN.DXT",text,sizeof(text),&n));assert(!strcmp(text,"furniture"));
 assert(game_write_file("data.p3","shared",6));assert(game_save_load(b)==SAVE_VALID);
 // Corrupt only the most recent commit; the previous independent slot survives.
 unsigned slot=game_save_generation()%2?0:1;flash[slot*65536+130]^=1;
 assert(game_save_load(b)==SAVE_VALID);
 assert(game_read_file("player6.p3",text,sizeof(text),&n)&&!strcmp(text,"sixth"));
}
