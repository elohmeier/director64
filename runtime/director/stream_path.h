#ifndef DIRECTOR64_STREAM_PATH_H
#define DIRECTOR64_STREAM_PATH_H
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// A virtual disc path, never a host filesystem path. Classic Mac's doubled
// colon walks to the parent; Windows and slash paths use ordinary components.
static bool dg_stream_path(const char *root, const char *folder, const char *path,
                           char *out, size_t cap) {
  char input[512], normalized[512];
  bool relative = path[0] == '@' || (!strchr(path, ':') && path[0] != '/' && path[0] != '\\');
  bool mac = strchr(path, ':') && !strchr(path, '\\') && !strchr(path, '/');
  const char *source = path[0] == '@' ? path + 1 : path;
  if (path[0] == '@' && (*source == ':' || *source == '\\' || *source == '/')) source++;
  int n = snprintf(input, sizeof(input), "%s%s%s", relative ? folder : "",
                   relative && *folder && folder[strlen(folder)-1] != '/' && folder[strlen(folder)-1] != '\\' && folder[strlen(folder)-1] != ':' ? "/" : "", source);
  if (n < 0 || n >= (int)sizeof(input)) return false;
  size_t used=0;
  for (const char *p=input; *p; p++) {
    if (*p == ':' && mac && p>input && (p[-1]==':' || p[-1]=='/' || p[-1]=='\\')) {
      if (used+3 >= sizeof(normalized)) return false;
      memcpy(normalized+used,"../",3); used+=3;
    } else {
      if (used+1 >= sizeof(normalized)) return false;
      normalized[used++] = *p==':' || *p=='\\' ? '/' : (char)tolower((unsigned char)*p);
    }
  }
  normalized[used]=0;
  char *parts[64]; unsigned count=0; bool rooted=false;
  char folded_root[64]; size_t root_length=strlen(root);
  if (!root_length || root_length>=sizeof(folded_root)) return false;
  for(size_t i=0;i<=root_length;i++) folded_root[i]=(char)tolower((unsigned char)root[i]);
  for (char *p=normalized; *p;) {
    while (*p=='/') p++;
    if (!*p) break;
    char *start=p; while(*p && *p!='/') p++;
    if(*p) *p++=0;
    if (!rooted) { if(!strcmp(start,folded_root)) rooted=true; continue; }
    if(!strcmp(start,".")) continue;
    if(!strcmp(start,"..")) {if(!count)return false; count--;continue;}
    if(count==64) return false;
    parts[count++]=start;
  }
  if(!rooted || !count) return false;
  char *dot=strrchr(parts[count-1],'.'); if(dot) *dot=0;
  used=0;
  for(unsigned i=0;i<count;i++) {
    size_t length=strlen(parts[i]);
    if(!length || used+length+(i!=0)>=cap) return false;
    if(i) out[used++]='/';
    memcpy(out+used,parts[i],length);used+=length;
  }
  out[used]=0; return true;
}
#endif
