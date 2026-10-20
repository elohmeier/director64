#ifndef DIRECTOR64_STREAM_NAME_H
#define DIRECTOR64_STREAM_NAME_H
#include <ctype.h>
#include <stddef.h>
#include <string.h>
// Resolve the disc's primary-volume 8.3 basename only after an exact match.
// Folder identity is retained: distinct speakers' equally named clips cannot
// resolve to one another. Keys are the converted lowercase stream namespace.
static inline const char *dg_stream_name(const char *const *keys,const char *name) {
  for(unsigned i=0;keys[i];i++) if(!strcmp(keys[i],name)) return keys[i];
  const char *base=strrchr(name,'/');base=base?base+1:name;
  size_t prefix=(size_t)(base-name);char short_name[256];
  if(prefix+9>sizeof(short_name))return NULL;
  memcpy(short_name,name,prefix);size_t n=prefix;
  for(const unsigned char *p=(const unsigned char *)base;*p && n<prefix+8;p++)
    if((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9'))short_name[n++]=(char)*p;
  short_name[n]=0;
  if(n==prefix)return NULL;
  for(unsigned i=0;keys[i];i++)if(!strcmp(keys[i],short_name))return keys[i];
  return NULL;
}
#endif
