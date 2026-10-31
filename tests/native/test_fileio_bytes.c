#include "director.h"
#include "user_files.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static const unsigned char bytes[]={0,0x80,0xff,'\r','\n','x'};
static unsigned char written[32];
static unsigned written_length;
static bool yielded;
static bool read_file(void *ctx,const char *name,char *out,unsigned cap,unsigned *length) {
  (void)ctx;(void)name;assert(cap>sizeof(bytes));
  memcpy(out,bytes,sizeof(bytes));out[sizeof(bytes)]=0;*length=sizeof(bytes);return true;
}
static bool write_file(void *ctx,const char *name,const char *data,unsigned length) {
  (void)ctx;(void)name;assert(length<=sizeof(written));
  memcpy(written,data,length);written_length=length;return true;
}
static lv_t text(const char *s) { return lv_text(&values,s,false); }
static lv_t invoke(const char *name,unsigned count,const lv_t *args) {
  lv_t result=0;yielded=false;
  assert(values.services.call(&values,name,count,args,&result,&yielded));assert(!values.failed);
  return result;
}
static void exact(lv_t value,const void *data,unsigned length) {
  assert(lv_byte_count(&values,value)==length);
  assert(!memcmp(lv_cstr(&values,value),data,length));
}
int main(void) {
  dg_init(&director,&values,(dg_platform_t){.read_file=read_file,.write_file=write_file},NULL,NULL,0,1);
  director.files[0].used=true;lv_t file=lv_make(LV_FILE,0);
  invoke("openFile",3,(lv_t[]){file,text("bytes.dat"),lv_num(1)});
  exact(invoke("readFile",1,&file),bytes,sizeof(bytes));
  invoke("setPosition",2,(lv_t[]){file,lv_num(0)});
  for(unsigned i=0;i<sizeof(bytes);i++) {
    lv_t value=invoke("readChar",1,&file);exact(value,bytes+i,1);
    lv_t result=lv_call_builtin(&values,NULL,"charToNum",1,&value);
    assert(lv_integer(&values,result)==bytes[i]);
  }
  invoke("setPosition",2,(lv_t[]){file,lv_num(0)});
  exact(invoke("readLine",1,&file),bytes,3);
  assert(lv_integer(&values,invoke("getPosition",1,&file))==5);
  exact(invoke("readFile",1,&file),bytes+5,1);
  invoke("openFile",3,(lv_t[]){file,text("bytes.dat"),lv_num(2)});
  lv_t binary=lv_bytes(&values,bytes,sizeof(bytes));
  invoke("writeString",2,(lv_t[]){file,binary});invoke("closeFile",1,&file);
  assert(written_length==sizeof(bytes) && !memcmp(written,bytes,sizeof(bytes)));

  // The legacy FileIO XObject has the same byte-preserving contract.
  lv_t legacy=invoke("fileio",3,(lv_t[]){text("mnew"),text("read"),text("bytes.dat")});
  exact(invoke("file_method",2,(lv_t[]){legacy,text("mreadfile")}),bytes,sizeof(bytes));
  invoke("file_method",2,(lv_t[]){legacy,text("mdispose")});
  legacy=invoke("fileio",3,(lv_t[]){text("mnew"),text("write"),text("bytes.dat")});
  invoke("file_method",3,(lv_t[]){legacy,text("mwritestring"),lv_bytes(&values,bytes,sizeof(bytes))});
  invoke("file_method",2,(lv_t[]){legacy,text("mdispose")});
  assert(written_length==sizeof(bytes) && !memcmp(written,bytes,sizeof(bytes)));

  // Selected raster/header bytes stay raw. A CAR is already normalized by
  // its host and retains UTF-8 character indexing through readFile.
  dg_user_reset();director.files[0].used=true;
  invoke("displayOpen",1,&file);unsigned id=dg_user_request()->id;
  assert(dg_user_reply(id,"bytes.bin",bytes,sizeof(bytes),NULL,0,0));
  assert(lv_run(&values,32));lv_t path=values.result;
  invoke("openFile",3,(lv_t[]){file,path,lv_num(1)});
  exact(invoke("readFile",1,&file),bytes,sizeof(bytes));
  invoke("setPosition",2,(lv_t[]){file,lv_num(0)});
  exact(invoke("readChar",1,&file),bytes,1);invoke("closeFile",1,&file);
  invoke("displaySave",3,(lv_t[]){file,text("Save"),text("bytes.bin")});id=dg_user_request()->id;
  assert(dg_user_reply(id,"bytes.bin",NULL,0,NULL,0,0));assert(lv_run(&values,32));path=values.result;
  invoke("createFile",2,(lv_t[]){file,path});invoke("openFile",3,(lv_t[]){file,path,lv_num(2)});
  invoke("writeString",2,(lv_t[]){file,lv_bytes(&values,bytes,sizeof(bytes))});
  invoke("closeFile",1,&file);const dg_user_request_t *request=dg_user_request();
  assert(yielded && request && request->size==sizeof(bytes) && !memcmp(request->data,bytes,sizeof(bytes)));
  assert(dg_user_complete(id,true));assert(lv_run(&values,32));
  invoke("displayOpen",1,&file);id=dg_user_request()->id;
  static const char german[]="M\303\274ller Stra\303\237e \342\202\254";
  assert(dg_user_reply(id,"german.car",(const uint8_t *)german,sizeof(german)-1,NULL,0,0));
  assert(lv_run(&values,32));path=values.result;
  invoke("openFile",3,(lv_t[]){file,path,lv_num(1)});
  lv_t utf8=invoke("readFile",1,&file);exact(utf8,german,sizeof(german)-1);
  assert(lv_integer(&values,lv_get(&values,NULL,"length",utf8))==15);
  invoke("closeFile",1,&file);
  dg_user_reset();
  puts("FileIO bytes: internal/XObject/selected reads and writes preserve NUL, high bytes, line delimiters and positions");
}
