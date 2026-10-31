#ifndef DIRECTOR64_JSON_TEXT_H
#define DIRECTOR64_JSON_TEXT_H
#include <stddef.h>
#include <stdint.h>

typedef void (*d64_json_write_fn)(void *,const char *,size_t);
static inline unsigned d64_json_utf8_span(const uint8_t *p,size_t remaining) {
  unsigned first=p[0],n=first>=0xc2 && first<=0xdf?2:first>=0xe0 && first<=0xef?3:
      first>=0xf0 && first<=0xf4?4:0;
  if(!n || n>remaining)return 0;
  unsigned scalar=first&((1u<<(7-n))-1u);
  for(unsigned i=1;i<n;i++) {
    if((p[i]&0xc0)!=0x80)return 0;
    scalar=(scalar<<6)|(p[i]&63);
  }
  if(scalar<(n==2?0x80u:n==3?0x800u:0x10000u) || scalar>0x10ffff ||
     (scalar>=0xd800 && scalar<=0xdfff))return 0;
  return n;
}
// Valid UTF-8 remains UTF-8. Isolated legacy bytes use their Windows-1252
// scalar, and explicit lengths preserve binary zeroes as JSON escapes.
static inline void d64_json_quote(const void *bytes,size_t length,
                                  d64_json_write_fn write,void *context) {
  static const uint16_t high[32]={0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
    0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,
    0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
  static const char digits[]="0123456789abcdef";
  const uint8_t *p=bytes;write(context,"\"",1);
  for(size_t at=0;at<length;) {
    unsigned c=p[at],span=c>=128?d64_json_utf8_span(p+at,length-at):0;
    if(span) {write(context,(const char *)p+at,span);at+=span;continue;}
    at++;
    if(c=='"' || c=='\\') {char escaped[2]={'\\',(char)c};write(context,escaped,2);}
    else if(c<32 || c>=127) {
      unsigned scalar=c>=128 && c<160?high[c-128]:c;
      char escaped[6]={'\\','u',digits[(scalar>>12)&15],digits[(scalar>>8)&15],
        digits[(scalar>>4)&15],digits[scalar&15]};
      write(context,escaped,6);
    } else {char ascii=(char)c;write(context,&ascii,1);}
  }
  write(context,"\"",1);
}
#endif
