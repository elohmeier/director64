// Optional source presentation descriptors; MOVI and RICH stay unchanged.
static bool load_text_boxes(loader_t *l) {
  reader_t *r=l->reader;
  unsigned records=count(r,12,65535,"text box member");
#if DG_CAP_TEXT_BOX
  for(unsigned i=0;i<records && !r->failed;i++) {
    uint32_t id=u32(r);dg_member_t *member=NULL;unsigned file=DG_ID_FILE(id);
    if(file && file<=l->package->movie_count) {
      const dg_movie_t *movie=l->package->registry[file-1];unsigned first=0,end=movie->member_count;
      while(first<end) {
        unsigned middle=first+(end-first)/2;uint32_t key=movie->members[middle].id;
        if(key==id){member=(dg_member_t *)&movie->members[middle];break;}
        if(key<id)first=middle+1;else end=middle;
      }
    }
    unsigned type=u8(r),margin=u8(r),border=u8(r),shadow=u8(r),wrap=u8(r);
    unsigned red=u8(r),green=u8(r),blue=u8(r);
    if(!member || (member->type!=3 && member->type!=7) || member->text_box_present ||
        type<1 || type>4 || wrap>1)
      return fail(r,"invalid, repeated or absent text box member");
    member->text_box_present=true;member->text_box_type=(uint8_t)type;
    member->text_margin=(uint8_t)margin;member->text_border=(uint8_t)border;
    member->text_box_shadow=(uint8_t)shadow;member->text_word_wrap=wrap!=0;
    member->text_box_bg=(uint32_t)red<<16|(uint32_t)green<<8|blue;
  }
#else
  if(records)return fail(r,"text box metadata requires extended text services");
#endif
  if(!r->failed && r->at!=r->length)return fail(r,"trailing text box bytes");
  return !r->failed;
}
