// Optional nonzero character spacing, keyed by an existing RICH UTF16 run.
static bool load_spacing(loader_t *l) {
  reader_t *r=l->reader;
  unsigned records=count(r,12,268431360,"character spacing run");
#if DG_MODERN && !DG_D5
  for(unsigned i=0;i<records && !r->failed;i++) {
    uint32_t id=u32(r),offset=u32(r);
    int16_t spacing=(int16_t)u16(r);
    unsigned reserved=u16(r),file=DG_ID_FILE(id);
    const dg_member_t *member=NULL;
    if(file && file<=l->package->movie_count) {
      const dg_movie_t *movie=l->package->registry[file-1];
      unsigned first=0,end=movie->member_count;
      while(first<end) {
        unsigned middle=first+(end-first)/2;
        uint32_t key=movie->members[middle].id;
        if(key==id) {member=&movie->members[middle];break;}
        if(key<id)first=middle+1;else end=middle;
      }
    }
    if(!member || !member->text_formats || !spacing || reserved)
      return fail(r,"invalid or absent character spacing member");
    dg_text_format_t *run=NULL;
    unsigned first=0,end=member->text_format_count;
    while(first<end) {
      unsigned middle=first+(end-first)/2;
      uint32_t key=member->text_formats[middle].offset;
      if(key==offset) {run=(dg_text_format_t *)&member->text_formats[middle];break;}
      if(key<offset)first=middle+1;else end=middle;
    }
    if(!run || run->char_spacing)
      return fail(r,"absent or repeated character spacing run");
    run->char_spacing=spacing;
  }
#else
  if(records)return fail(r,"character spacing requires a modern Director profile");
#endif
  if(!r->failed && r->at!=r->length)return fail(r,"trailing character spacing bytes");
  return !r->failed;
}
