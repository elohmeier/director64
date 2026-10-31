#include "lingo_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t r;
static lv_t call(const char *name,unsigned n,const lv_t *args) {
  lv_t v=lv_call_builtin(&r,NULL,name,n,args);assert(!r.failed);return v;
}
static lv_t character(lv_t text,unsigned n) {
  return lv_chunk(&r,"char",text,lv_num(n),lv_num(n));
}
int main(void) {
  lv_init(&r,(lv_services_t){0},NULL,NULL,0,1);
  r.roots[0]=lv_text(&r,"grün €ß",false);
  assert(lv_count(&r,r.roots[0])==7 && lv_byte_count(&r,r.roots[0])==11);
  assert(!strcmp(lv_cstr(&r,character(r.roots[0],3)),"ü"));
  assert(lv_numeric(call("chartonum",1,(lv_t[]){character(r.roots[0],6)}))==128);
  assert(lv_numeric(call("offset",2,(lv_t[]){lv_text(&r,"€",false),r.roots[0]}))==6);
  assert(lv_truth(&r,lv_binary(&r,"contains",r.roots[0],lv_text(&r,"ÜN",false))));
  r.roots[1]=call("numtochar",1,(lv_t[]){lv_num(228)});
  assert(lv_count(&r,r.roots[1])==1 && lv_byte_count(&r,r.roots[1])==1);
  assert(!strcmp(lv_cstr(&r,lv_text_utf8(&r,r.roots[1])),"ä"));
  assert(lv_truth(&r,lv_binary(&r,"=",r.roots[1],lv_text(&r,"Ä",false))));
  // Authored list2Name/UTF8 encoders require these to remain two bytes.
  r.roots[2]=lv_binary(&r,"&",call("numtochar",1,(lv_t[]){lv_num(195)}),
                                  call("numtochar",1,(lv_t[]){lv_num(164)}));
  assert(lv_count(&r,r.roots[2])==2 && lv_byte_count(&r,r.roots[2])==2);
  assert(lv_numeric(call("chartonum",1,(lv_t[]){character(r.roots[2],2)}))==164);
  // A return from numToChar mixed with package German text becomes legacy
  // byte text, then normalizes once when assigned to a displayed member.
  r.roots[3]=lv_binary(&r,"&",r.roots[0],call("numtochar",1,(lv_t[]){lv_num(13)}));
  assert(lv_byte_count(&r,r.roots[3])==8);
  assert(!strcmp(lv_cstr(&r,lv_text_utf8(&r,r.roots[3])),"grün €ß\r"));
  r.roots[4]=lv_chunk_set(&r,"char",r.roots[0],lv_num(3),lv_num(3),r.roots[1]);
  assert(!strcmp(lv_cstr(&r,r.roots[4]),"grän €ß"));
  r.roots[5]=lv_chunk_delete(&r,"char",r.roots[4],lv_num(6),lv_num(6));
  assert(!strcmp(lv_cstr(&r,r.roots[5]),"grän ß"));
  const unsigned char data[]={0,255,195,164,0,128};
  r.roots[6]=lv_bytes(&r,data,sizeof(data));
  r.roots[7]=lv_chunk_set(&r,"char",r.roots[6],lv_num(2),lv_num(2),lv_bytes(&r,"\0",1));
  const unsigned char expected[]={0,0,195,164,0,128};
  assert(lv_count(&r,r.roots[7])==sizeof(expected));
  assert(!memcmp(lv_cstr(&r,r.roots[7]),expected,sizeof(expected)));
  r.roots[8]=lv_binary(&r,"&",r.roots[6],r.roots[6]);
  lv_collect(&r);
  assert(lv_count(&r,r.roots[8])==12 && !memcmp(lv_cstr(&r,r.roots[8])+6,data,6));
  assert(lv_count(&r,r.roots[0])==7 && lv_count(&r,r.roots[2])==2);
  lv_t expanded=lv_chunk_set(&r,"line",lv_text(&r,"",false),lv_num(3),lv_num(3),lv_text(&r,"X",false));
  assert(!strcmp(lv_cstr(&r,expanded),"\r\rX"));
  expanded=lv_chunk_set(&r,"line",lv_text(&r,"one\r",false),lv_num(3),lv_num(3),lv_text(&r,"X",false));
  assert(!strcmp(lv_cstr(&r,expanded),"one\r\rX"));
  expanded=lv_chunk_set(&r,"item",lv_text(&r,"one",false),lv_num(3),lv_num(3),lv_text(&r,"X",false));
  assert(!strcmp(lv_cstr(&r,expanded),"one,,X"));
  expanded=lv_chunk_set(&r,"char",lv_text(&r,"äb",false),lv_num(5),lv_num(5),lv_text(&r,"X",false));
  assert(!strcmp(lv_cstr(&r,expanded),"äb  X"));
  expanded=lv_chunk_set(&r,"word",lv_text(&r,"one",false),lv_num(3),lv_num(3),lv_text(&r,"X",false));
  assert(!strcmp(lv_cstr(&r,expanded),"oneX"));
  // Truncated input has a bounded one-byte fallback.
  assert(lv_count(&r,lv_text(&r,"\xf0\x9f",false))==2);
  assert(!r.failed);
  puts("UTF-8 text indexing and Western binary strings survive edits and GC");
}
