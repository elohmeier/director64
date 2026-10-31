// Optional per-run formatting. Offsets match host text APIs: UTF-16 units.
static bool load_rich(loader_t *l) {
  reader_t *r = l->reader;
  unsigned records = count(r, 24, 65535, "rich text member");
#if DG_MODERN && !DG_D5
  for (unsigned i=0;i<records && !r->failed;i++) {
    uint32_t id=u32(r);
    dg_member_t *member=NULL;
    unsigned file=DG_ID_FILE(id);
    if(file && file<=l->package->movie_count) {
      const dg_movie_t *movie=l->package->registry[file-1];
      unsigned first=0,end=movie->member_count;
      while(first<end) {
        unsigned middle=first+(end-first)/2;
        uint32_t key=movie->members[middle].id;
        if(key==id) {member=(dg_member_t *)&movie->members[middle];break;}
        if(key<id)first=middle+1;else end=middle;
      }
    }
    unsigned runs=u16(r),reserved=u16(r);
    if(!member || (member->type!=3 && member->type!=7) || member->text_formats ||
       !runs || runs>4096 || reserved || !need(r,(size_t)runs*16))
      return fail(r,"invalid, repeated or absent rich text member");
    unsigned extent=0;
    for(const unsigned char *p=(const unsigned char *)member->text;*p;p++)
      if((*p&0xc0)!=0x80)extent+=*p>=0xf0?2:1;
    dg_text_format_t *formats=allocate(l,runs,sizeof(*formats));
    if(!formats)return false;
    for(unsigned j=0;j<runs && !r->failed;j++) {
      dg_text_format_t *run=&formats[j];
      run->offset=u32(r);run->font=u8(r);run->size=u8(r);
      run->face=u8(r);run->align=u8(r);
      run->ascent=(int16_t)u16(r);run->line_height=(int16_t)u16(r);run->rgb=u32(r);
      if(run->offset>extent || (!j && run->offset) || (j && run->offset<=formats[j-1].offset) ||
         run->face>15 || run->align>3 ||
         run->line_height<0 || run->rgb>0xffffff)
        return fail(r,"invalid rich text format run");
    }
    member->text_format_count=(uint16_t)runs;member->text_formats=formats;
  }
#else
  if(records)return fail(r,"rich text runs require a modern Director profile");
#endif
  if(!r->failed && r->at!=r->length)return fail(r,"trailing rich text format bytes");
  return !r->failed;
}
