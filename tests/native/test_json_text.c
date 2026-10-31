#include "json_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char output[1024];
static size_t used;
static void append(void *ctx,const char *text,size_t length) {
  (void)ctx;assert(length<sizeof(output)-used);
  memcpy(output+used,text,length);used+=length;output[used]=0;
}
static void check(const void *data,size_t length,const char *expected) {
  used=0;d64_json_quote(data,length,append,NULL);
  if(strcmp(output,expected))fprintf(stderr,"JSON mismatch: %s != %s\n",output,expected);
  assert(!strcmp(output,expected));
}
int main(void) {
  const char german[]="M\303\274ller Stra\303\237e \342\202\254 \360\237\232\227";
  char expected[128];snprintf(expected,sizeof(expected),"\"%s\"",german);
  check(german,strlen(german),expected);
  const unsigned char legacy[]={0xe4,'|',0xdf,'|',0x80};
  check(legacy,sizeof(legacy),"\"\\u00e4|\\u00df|\\u20ac\"");
  const unsigned char valid_pair[]={0xdf,0x80};check(valid_pair,2,"\"\337\200\"");
  const unsigned char binary[]={'A',0,0xff,'"','\\','\n',0x81};
  check(binary,sizeof(binary),"\"A\\u0000\\u00ff\\\"\\\\\\u000a\\u0081\"");
  const unsigned char truncated[]={0xc3};check(truncated,1,"\"\\u00c3\"");
  const unsigned char overlong[]={0xc0,0xaf};check(overlong,2,"\"\\u00c0\\u00af\"");
  check(NULL,0,"\"\"");
  puts("JSON text: valid UTF8, legacy Windows1252, exact binary NUL, escaping and truncated sequences pass");
}
