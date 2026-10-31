static unsigned format_name_id(lv_runtime_t *r,const char *name) {
  if(eq(name,"textfont"))return LN_FONT;
  if(eq(name,"textsize"))return LN_FONTSIZE;
  if(eq(name,"textstyle"))return LN_FONTSTYLE;
  if(eq(name,"textcolor"))return LN_COLOR;
  return lv_name_id(r,name);
}
// Initial source runs plus bounded, member-owned formatting overlays. The
// compiler preserves a chunk reference only for formatting property access;
// ordinary chunk expressions remain strings.
static bool format_name_equal(const char *a,const char *b) {
  size_t an=strlen(a),bn=strlen(b);
  while(an&&(a[an-1]=='*'||a[an-1]==' '))an--;
  while(bn&&(b[bn-1]=='*'||b[bn-1]==' '))bn--;
  if(an!=bn)return false;
  for(size_t i=0;i<an;i++)if(tolower((unsigned char)a[i])!=tolower((unsigned char)b[i]))return false;
  return true;
}
static const dg_text_style_t *format_font(dg_runtime_t *d,const char *name,unsigned id,unsigned size) {
  const dg_text_style_t *best=NULL;unsigned distance=UINT32_MAX;
  for(unsigned file=0;file<d->loaded_count;file++) {
    const dg_movie_t *movie=d->loaded[file];
    for(unsigned j=0;movie&&j<movie->member_count;j++) {
      const dg_member_t *m=&movie->members[j];const dg_text_style_t *styles[]={m->text_style,m->text_insert_style};
      for(unsigned k=0;k<2;k++)if(styles[k]&&styles[k]->font_id&&
          (name?(styles[k]->font_name&&format_name_equal(styles[k]->font_name,name)):styles[k]->font_id==id)) {
        unsigned delta=styles[k]->size>size?styles[k]->size-size:size-styles[k]->size;
        if(delta<distance){best=styles[k];distance=delta;}
      }
    }
  }
  return best;
}
static unsigned format_units(const char *text,size_t bytes) {
  unsigned count=0;
  for(size_t i=0;i<bytes&&text[i];) {
    const unsigned char *p=(const unsigned char *)text+i;size_t n=1;unsigned units=1;
    if(p[0]>=0xc2&&p[0]<=0xdf&&i+1<bytes&&(p[1]&0xc0)==0x80)n=2;
    else if(p[0]>=0xe0&&p[0]<=0xef&&i+2<bytes&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80)n=3;
    else if(p[0]>=0xf0&&p[0]<=0xf4&&i+3<bytes&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80&&(p[3]&0xc0)==0x80){n=4;units=2;}
    i+=n;count+=units;
  }
  return count;
}
static size_t format_byte(const char *text,unsigned units) {
  size_t i=0;unsigned count=0;
  while(text[i]&&count<units) {
    size_t n=1;const unsigned char *p=(const unsigned char *)text+i;
    if(p[0]>=0xc2&&p[0]<=0xdf&&p[1]&&(p[1]&0xc0)==0x80)n=2;
    else if(p[0]>=0xe0&&p[0]<=0xef&&p[1]&&p[2]&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80)n=3;
    else if(p[0]>=0xf0&&p[0]<=0xf4&&p[1]&&p[2]&&p[3]&&(p[1]&0xc0)==0x80&&(p[2]&0xc0)==0x80&&(p[3]&0xc0)==0x80)n=4;
    count+=n==4?2:1;i+=n;
  }
  return i;
}
static unsigned format_pool(dg_runtime_t *d,uint32_t member,unsigned *count) {
  unsigned start=0;while(start<d->text_format_count&&d->text_format_members[start]!=member)start++;
  unsigned end=start;while(end<d->text_format_count&&d->text_format_members[end]==member)end++;
  *count=end-start;return start;
}
const dg_text_format_t *dg_text_formats(dg_runtime_t *d,uint32_t member,unsigned *count) {
  unsigned start=format_pool(d,member,count);return *count?d->text_format_runs+start:NULL;
}
static void format_clear(dg_runtime_t *d,uint32_t member) {
  unsigned at=0;for(unsigned i=0;i<d->text_format_count;i++)if(d->text_format_members[i]!=member) {
    d->text_format_members[at]=d->text_format_members[i];d->text_format_runs[at++]=d->text_format_runs[i];
  }
  d->text_format_count=(uint16_t)at;
}
static dg_text_format_t format_base(dg_runtime_t *d,const dg_member_t *m,unsigned fi) {
  const dg_text_style_t *s=d->field_text_changed[fi]&&m->text_insert_style?m->text_insert_style:m->text_style;
  dg_text_format_t f={.font=s?s->font_id:0,.size=s?s->size:12,.align=s?s->align:0,
    .ascent=s?s->ascent:12,.line_height=s?s->line_height:15,.rgb=s?s->color:0};
  if(m->text_format_count) {
    f.face=m->text_formats[d->field_text_changed[fi]?m->text_format_count-1:0].face;
    f.char_spacing=m->text_formats[0].char_spacing;
  }
  return f;
}
static void format_globals(dg_runtime_t *d,unsigned fi,dg_text_format_t *f) {
  if(d->field_font_changed[fi]) {
    const dg_text_style_t *s=format_font(d,d->field_fonts[fi],0,f->size?f->size:12);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    if(!s)s=format_font(d,"Arial",0,f->size?f->size:12);
#endif
    if(!s){error(d,"text range font unavailable",d->field_fonts[fi]);return;}
    unsigned size=s->size?s->size:12;f->font=s->font_id;
    unsigned effective=f->size?f->size:12;
    f->ascent=(int16_t)((s->ascent*effective+(int)size/2)/(int)size);
    f->line_height=(int16_t)((s->line_height*effective+(int)size/2)/(int)size);
  }
  if(d->field_size_changed[fi]) {
    unsigned old=f->size?f->size:12;f->size=d->field_sizes[fi];unsigned effective=f->size?f->size:12;
    f->ascent=(int16_t)((f->ascent*effective+(int)old/2)/(int)old);
    f->line_height=(int16_t)((f->line_height*effective+(int)old/2)/(int)old);
  }
  if(d->field_align[fi])f->align=d->field_align[fi]-1;
  if(d->field_face_changed[fi])f->face=d->field_faces[fi];
  if(d->field_line_space[fi]>0)f->line_height=d->field_line_space[fi];
  if(d->field_spacing_changed[fi])f->char_spacing=d->field_char_spacing[fi];
  lv_t color=d->values->roots[DG_FIELD_COLOR_ROOT+fi];
  if(lv_type(color)!=LV_VOID) {
    unsigned n=(unsigned)lv_integer(d->values,color);
    const uint32_t *palette=dg_stage_palette(d);
    f->rgb=lv_type(color)==LV_COLOR||n>255||!palette?n:palette[n];
  }
}
static unsigned format_collect(dg_runtime_t *d,const dg_member_t *m,unsigned fi,dg_text_format_t *out) {
  unsigned count=0;const dg_text_format_t *live=dg_text_formats(d,m->id,&count);
  if(live){memcpy(out,live,count*sizeof(*out));return count;}
  if(!d->field_text_changed[fi]&&m->text_format_count) {
    count=m->text_format_count;if(count>1024){error(d,"mutable text formatting capacity","over 1024 runs");return 0;}
    memcpy(out,m->text_formats,count*sizeof(*out));
  } else {out[0]=format_base(d,m,fi);count=1;}
  for(unsigned i=0;i<count;i++)format_globals(d,fi,&out[i]);
  return d->values->failed?0:count;
}
static unsigned format_at(const dg_text_format_t *f,unsigned count,unsigned offset) {
  unsigned i=0;while(i+1<count&&f[i+1].offset<=offset)i++;return i;
}
static void format_boundary(dg_text_format_t *f,unsigned *count,unsigned offset) {
  unsigned i=format_at(f,*count,offset);if(f[i].offset==offset)return;
  memmove(f+i+2,f+i+1,(*count-i-1)*sizeof(*f));f[i+1]=f[i];f[i+1].offset=offset;(*count)++;
}
static bool format_same(const dg_text_format_t *a,const dg_text_format_t *b) {
  return a->rgb==b->rgb&&a->font==b->font&&a->size==b->size&&a->face==b->face&&a->align==b->align&&
    a->ascent==b->ascent&&a->line_height==b->line_height&&a->char_spacing==b->char_spacing;
}
static unsigned format_faces(dg_runtime_t *d,lv_t value) {
  unsigned face=0;
  if(lv_type(value)==LV_STRING) {
    const char *p=lv_cstr(d->values,value);
    while(*p) {
      while(*p==' '||*p==',')p++;
      const char *start=p;while(*p&&*p!=',')p++;
      size_t length=(size_t)(p-start);while(length&&start[length-1]==' ')length--;
      char style[32];if(length>=sizeof(style)){error(d,"unsupported text fontStyle",start);return 0;}
      memcpy(style,start,length);style[length]=0;
      if(eq(style,"bold"))face|=1;else if(eq(style,"italic"))face|=2;
      else if(eq(style,"underline"))face|=4;else if(eq(style,"strikeout"))face|=8;
      else if(!eq(style,"plain")){error(d,"unsupported text fontStyle",style);return 0;}
    }
    return face;
  }
  if(lv_type(value)!=LV_LIST){error(d,"fontStyle requires a style list","text range");return 0;}
  for(unsigned i=1;i<=lv_count(d->values,value);i++) {
    const char *s=lv_cstr(d->values,lv_at(d->values,value,i));
    if(eq(s,"bold"))face|=1;else if(eq(s,"italic"))face|=2;else if(eq(s,"underline"))face|=4;
    else if(eq(s,"strikeout"))face|=8;else if(!eq(s,"plain"))error(d,"unsupported text fontStyle",s);
  }
  return face;
}
static bool format_property(unsigned ln) {
  return ln==LN_FONT||ln==LN_FONTSIZE||ln==LN_FONTSTYLE||ln==LN_ALIGNMENT||ln==LN_COLOR||
    ln==LN_FORECOLOR||ln==LN_CHARSPACING||ln==LN_FIXEDLINESPACE;
}
static bool format_apply(dg_runtime_t *d,const dg_member_t *m,unsigned start,unsigned end,unsigned ln,lv_t value) {
  static dg_text_format_t runs[1026];int fi=field(d,m->id);if(fi<0)return false;
  unsigned count=format_collect(d,m,(unsigned)fi,runs);if(!count)return false;
  format_boundary(runs,&count,start);format_boundary(runs,&count,end);
  const dg_text_style_t *font=NULL;unsigned face=0;
  int n=ln==LN_FONT||ln==LN_FONTSTYLE||ln==LN_ALIGNMENT?0:lv_integer(d->values,value);
  if(ln==LN_FONT) {
    const char *name=lv_cstr(d->values,value);font=format_font(d,name,0,runs[format_at(runs,count,start)].size);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    if(!font)font=format_font(d,"Arial",0,runs[format_at(runs,count,start)].size);
#endif
    if(!font){error(d,"text range font unavailable",name);return false;}
    if(!start)snprintf(d->field_fonts[fi],sizeof(d->field_fonts[fi]),"%s",name);
  }
  if(ln==LN_FONTSTYLE)face=format_faces(d,value);
  if(d->values->failed)return false;
  if((ln==LN_FONTSIZE&&(n<0||n>255))||(ln==LN_FIXEDLINESPACE&&(n<0||n>4096))||
      (ln==LN_CHARSPACING&&(n<INT16_MIN||n>INT16_MAX))) {
    error(d,"text formatting value out of range","text range");return false;
  }
  for(unsigned i=0;i<count&&!d->values->failed;i++)if(runs[i].offset>=start&&
      (runs[i].offset<end||(start==end&&runs[i].offset==start))) {
    dg_text_format_t *f=&runs[i];
    if(ln==LN_FONT) {
      unsigned base=font->size?font->size:12,effective=f->size?f->size:12;f->font=font->font_id;
      f->ascent=(int16_t)((font->ascent*effective+(int)base/2)/(int)base);
      if(!d->field_line_space[fi])f->line_height=(int16_t)((font->line_height*effective+(int)base/2)/(int)base);
    } else if(ln==LN_FONTSIZE) {
      unsigned old=f->size?f->size:12,effective=n?(unsigned)n:12;f->size=(uint8_t)n;
      f->ascent=(int16_t)((f->ascent*(int)effective+(int)old/2)/(int)old);
      if(!d->field_line_space[fi])f->line_height=(int16_t)((f->line_height*(int)effective+(int)old/2)/(int)old);
    } else if(ln==LN_FONTSTYLE)f->face=(uint8_t)face;
    else if(ln==LN_ALIGNMENT)f->align=eq(lv_cstr(d->values,value),"center")?1:eq(lv_cstr(d->values,value),"right")?2:eq(lv_cstr(d->values,value),"full")?3:0;
    else if(ln==LN_CHARSPACING)f->char_spacing=(int16_t)n;
    else if(ln==LN_FIXEDLINESPACE) {
      unsigned effective=f->size?f->size:12;
      const dg_text_style_t *s=format_font(d,NULL,f->font,effective);unsigned base=s&&s->size?s->size:12;
      int natural=s?s->ascent+s->descent+s->leading:(int)effective*6/5;
      f->line_height=(int16_t)(n?n:(natural*(int)effective+(int)base/2)/(int)base);
    } else {
      const uint32_t *palette=dg_stage_palette(d);
      f->rgb=lv_type(value)==LV_COLOR||n>255||n<0||!palette?(uint32_t)n:palette[n];
    }
  }
  unsigned kept=0;
  for(unsigned i=0;i<count;i++)if(!kept||!format_same(&runs[kept-1],&runs[i]))runs[kept++]=runs[i];
  unsigned old=0;(void)format_pool(d,m->id,&old);
  if(d->text_format_count-old+kept>1024){error(d,"mutable text formatting capacity","over 1024 runs");return false;}
  format_clear(d,m->id);
  for(unsigned i=0;i<kept;i++) {
    unsigned at=d->text_format_count++;d->text_format_members[at]=m->id;d->text_format_runs[at]=runs[i];
  }
  d->visual_revision++;return !d->values->failed;
}
static bool format_proxy(dg_runtime_t *d,lv_t owner,uint32_t *member,unsigned *start,unsigned *end,unsigned depth) {
  if(lv_type(owner)!=LV_PROPLIST)return false;
  lv_runtime_t *r=d->values;lv_t target={0},first=lv_num(1),last=lv_num(1);const char *kind="char";
  // Ordinary property lists (including Flash object bags) can contain numeric
  // keys and arbitrary `kind` values. Recognize our private owner first.
  for(unsigned i=1;i<=lv_count(r,owner);i++) {
    lv_t key=lv_at(r,owner,i*2-1);
    if((lv_type(key)==LV_STRING||lv_type(key)==LV_SYMBOL) && eq(lv_cstr(r,key),"_textRange"))
      target=lv_at(r,owner,i*2);
  }
  if(lv_type(target)==LV_VOID)return false;
  for(unsigned i=1;i<=lv_count(r,owner);i++) {
    lv_t key_value=lv_at(r,owner,i*2-1);
    if(lv_type(key_value)!=LV_STRING && lv_type(key_value)!=LV_SYMBOL)continue;
    const char *key=lv_cstr(r,key_value);lv_t value=lv_at(r,owner,i*2);
    if(eq(key,"_textRange"))target=value;
    else if(eq(key,"kind"))kind=lv_cstr(r,value);
    else if(eq(key,"first"))first=value;else if(eq(key,"last"))last=value;
  }
  if(depth>=8){error(d,"nested text formatting range capacity","over 8 levels");return true;}
  unsigned parent_start=0,parent_end=0;
  if(lv_type(target)==LV_MEMBER||lv_type(target)==LV_FIELD)*member=(uint32_t)lv_id(target);
  else if(!format_proxy(d,target,member,&parent_start,&parent_end,depth+1)) {
    error(d,"invalid text formatting range target","expected member");return true;
  }
  const dg_member_t *m=dg_member(d,*member);if(!m||(m->type!=3&&m->type!=7)){error(d,"invalid text formatting member","expected text or field");return true;}
  int fi=field(d,*member);if(fi<0)return true;
  lv_t source=r->roots[DG_FIELD_TEXT_ROOT+fi];const char *text=lv_cstr(r,source);
  if(lv_type(target)==LV_PROPLIST) {
    size_t a=format_byte(text,parent_start),b=format_byte(text,parent_end);char *part=malloc(b-a+1);
    if(!part){error(d,"text range allocation","failed");return true;}
    memcpy(part,text+a,b-a);part[b-a]=0;source=lv_text(r,part,false);free(part);text=lv_cstr(r,source);
  }
  size_t a=0,b=0;
  if(eq(kind,"char")) {
    int lo=lv_integer(r,first),hi=lv_integer(r,last);unsigned extent=format_units(text,strlen(text));
    if(lo==-30000)lo=hi=(int)extent;
    if(lo<1){a=0;b=strlen(text);}else {
      if(hi<1)hi=lo;
      a=format_byte(text,(unsigned)lo-1);b=format_byte(text,(unsigned)hi);
      if(b<a)b=a;
    }
  } else (void)lv_chunk_range(r,kind,source,first,last,&a,&b);
  *start=parent_start+format_units(text,a);*end=parent_start+format_units(text,b);return true;
}
static unsigned format_palette_index(dg_runtime_t *d,uint32_t rgb) {
  const uint32_t *palette=dg_stage_palette(d);unsigned best=255;uint32_t distance=UINT32_MAX;
  for(unsigned i=0;palette&&i<256;i++) {
    uint32_t p=palette[i];int red=(int)(p>>16&255)-(int)(rgb>>16&255);
    int green=(int)(p>>8&255)-(int)(rgb>>8&255),blue=(int)(p&255)-(int)(rgb&255);
    uint32_t delta=(uint32_t)(red*red+green*green+blue*blue);
    if(delta<distance){best=i;distance=delta;}
  }
  return best;
}
static bool format_hilite(dg_runtime_t *d,lv_t owner) {
  uint32_t member=0;unsigned start=0,end=0;
  if(lv_type(owner)==LV_MEMBER||lv_type(owner)==LV_FIELD) {
    member=(uint32_t)lv_id(owner);const dg_member_t *m=dg_member(d,member);
    if(!m||(m->type!=3&&m->type!=7)){error(d,"hilite requires a text member","hilite");return true;}
    int fi=field(d,member);if(fi<0)return true;
    const char *text=lv_cstr(d->values,d->values->roots[DG_FIELD_TEXT_ROOT+fi]);
    end=format_units(text,strlen(text));
  } else if(!format_proxy(d,owner,&member,&start,&end,0)) {
    error(d,"hilite requires a retained member or chunk reference","hilite");return true;
  }
  if(d->values->failed)return true;
  if(d->hilite_member!=member||d->hilite_start!=start||d->hilite_end!=end)d->visual_revision++;
  d->hilite_member=member;d->hilite_start=start;d->hilite_end=end;
#if DG_EXTENDED
  d->sel_start=start;d->sel_end=end;
#endif
  return true;
}
static lv_t format_face_value(lv_runtime_t *r,bool classic,unsigned face) {
  static const char *const names[]={"bold","italic","underline","strikeout"};
  if(classic) {
    char text[64]="";unsigned n=0;
    for(unsigned i=0;i<4;i++)if(face&(1u<<i))n+=(unsigned)snprintf(text+n,sizeof(text)-n,"%s%s",n?",":"",names[i]);
    return lv_text(r,n?text:"plain",false);
  }
  lv_t styles[4];unsigned n=0;
  for(unsigned i=0;i<4;i++)if(face&(1u<<i))styles[n++]=lv_symbol(r,names[i]);
  if(!n)styles[n++]=lv_symbol(r,"plain");
  return lv_list(r,n,styles,false);
}
bool dg_text_format_property_get(lv_runtime_t *r,lv_t owner,const char *name,lv_t *out) {
  dg_runtime_t *d=r->context;unsigned ln=format_name_id(r,name);if(!format_property(ln))return false;
  uint32_t member=0;unsigned start=0,end=0;if(!format_proxy(d,owner,&member,&start,&end,0))return false;
  if(r->failed){*out=(lv_t){0};return true;}
  const dg_member_t *m=dg_member(d,member);static dg_text_format_t runs[1026];int fi=field(d,member);
  unsigned count=format_collect(d,m,(unsigned)fi,runs);if(!count){*out=(lv_t){0};return true;}
  const dg_text_format_t *f=&runs[format_at(runs,count,start)];
  if(ln==LN_FONT) {
    const dg_text_style_t *s=format_font(d,NULL,f->font,f->size?f->size:12);*out=lv_text(r,s&&s->font_name?s->font_name:d->field_fonts[fi],false);
  } else if(ln==LN_FONTSTYLE) {
    *out=format_face_value(r,m->text_box_present,f->face);
  } else if(ln==LN_ALIGNMENT) {
    const char *value=f->align==1?"center":f->align==2?"right":f->align==3?"full":"left";
    *out=m->text_box_present?lv_text(r,value,false):lv_symbol(r,value);
  }
  else if(ln==LN_FORECOLOR)*out=lv_num(format_palette_index(d,f->rgb));
  else if(ln==LN_COLOR)*out=lv_make(LV_COLOR,f->rgb);
  else *out=lv_num(ln==LN_FONTSIZE?f->size:ln==LN_CHARSPACING?f->char_spacing:f->line_height);
  return true;
}
bool dg_text_format_property_set(lv_runtime_t *r,lv_t owner,const char *name,lv_t value) {
  dg_runtime_t *d=r->context;unsigned ln=format_name_id(r,name);if(!format_property(ln))return false;
  uint32_t member=0;unsigned start=0,end=0;if(!format_proxy(d,owner,&member,&start,&end,0))return false;
  if(!r->failed)(void)format_apply(d,dg_member(d,member),start,end,ln,value);
  return true;
}
static bool format_member_set(dg_runtime_t *d,uint32_t member,unsigned ln,lv_t value) {
  if(!format_property(ln))return false;
  const dg_member_t *m=dg_member(d,member);if(!m||(m->type!=3&&m->type!=7))return false;
  unsigned count=0;(void)format_pool(d,member,&count);
  if(!count&&ln!=LN_CHARSPACING&&ln!=LN_FIXEDLINESPACE&&
      !(ln==LN_FONTSIZE&&lv_integer(d->values,value)==0)&&
      !(ln==LN_FONTSTYLE&&m->text_box_present&&lv_type(value)==LV_STRING)&&
      !(ln==LN_ALIGNMENT&&(eq(lv_cstr(d->values,value),"full"))))return false;
  int fi=field(d,member);if(fi<0)return true;
  const char *text=lv_cstr(d->values,d->values->roots[DG_FIELD_TEXT_ROOT+fi]);unsigned extent=format_units(text,strlen(text));
  if(!format_apply(d,m,0,extent,ln,value))return true;
  if(ln==LN_FIXEDLINESPACE)d->field_line_space[fi]=(int16_t)lv_integer(d->values,value);
  else if(ln==LN_CHARSPACING){d->field_char_spacing[fi]=(int16_t)lv_integer(d->values,value);d->field_spacing_changed[fi]=true;}
  else if(ln==LN_FONTSIZE){d->field_sizes[fi]=(uint8_t)lv_integer(d->values,value);d->field_size_changed[fi]=true;}
  else if(ln==LN_ALIGNMENT)d->field_align[fi]=eq(lv_cstr(d->values,value),"center")?2:eq(lv_cstr(d->values,value),"right")?3:eq(lv_cstr(d->values,value),"full")?4:1;
  else if(ln==LN_FONTSTYLE){d->field_faces[fi]=(uint8_t)format_faces(d,value);d->field_face_changed[fi]=true;}
  else if(ln==LN_FONT){snprintf(d->field_fonts[fi],sizeof(d->field_fonts[fi]),"%s",lv_cstr(d->values,value));d->field_font_changed[fi]=true;}
  else if(ln==LN_COLOR||ln==LN_FORECOLOR)d->values->roots[DG_FIELD_COLOR_ROOT+fi]=value;
  return true;
}
static bool format_member_get(dg_runtime_t *d,uint32_t member,unsigned ln,lv_t *out) {
  if(!format_property(ln))return false;
  const dg_member_t *m=dg_member(d,member);if(!m||(m->type!=3&&m->type!=7))return false;
  unsigned count=0;const dg_text_format_t *formats=dg_text_formats(d,member,&count);
  lv_runtime_t *r=d->values;int fi=field(d,member);if(fi<0)return true;
  dg_text_format_t initial;
  if(!count) {
    initial=!d->field_text_changed[fi]&&m->text_format_count?m->text_formats[0]:format_base(d,m,(unsigned)fi);
    if(d->field_size_changed[fi])initial.size=d->field_sizes[fi];
    if(d->field_align[fi])initial.align=d->field_align[fi]-1;
    if(d->field_face_changed[fi])initial.face=d->field_faces[fi];
    if(d->field_spacing_changed[fi])initial.char_spacing=d->field_char_spacing[fi];
    lv_t color=r->roots[DG_FIELD_COLOR_ROOT+fi];
    if(lv_type(color)!=LV_VOID) {
      unsigned n=(unsigned)lv_integer(r,color);const uint32_t *palette=dg_stage_palette(d);
      initial.rgb=lv_type(color)==LV_COLOR||n>255||!palette?n:palette[n];
    }
    formats=&initial;
  }
  const dg_text_format_t *f=formats;
  if(ln==LN_FONT){*out=lv_text(r,d->field_fonts[fi],false);return true;}
  if(ln==LN_FIXEDLINESPACE){*out=lv_num(d->field_line_space[fi]);return true;}
  if(ln==LN_FONTSTYLE) {
    *out=format_face_value(r,m->text_box_present,f->face);return true;
  }
  if(ln==LN_ALIGNMENT) {
    const char *value=f->align==1?"center":f->align==2?"right":f->align==3?"full":"left";
    *out=m->text_box_present?lv_text(r,value,false):lv_symbol(r,value);
  }
  else if(ln==LN_FORECOLOR)*out=lv_num(format_palette_index(d,f->rgb));
  else if(ln==LN_COLOR)*out=lv_make(LV_COLOR,f->rgb);
  else *out=lv_num(ln==LN_FONTSIZE?f->size:f->char_spacing);
  return true;
}
// Carry character formatting through put-before/after/into and deletion.
// `newfulltext` is already constructed by the VM. It remains rooted by the
// field on success; the VM keeps the original member reference in its owner.
static void format_piece(const dg_text_format_t *source,unsigned count,unsigned begin,unsigned end,
                         unsigned destination,dg_text_format_t *out,unsigned *used) {
  if(begin>=end)return;
  dg_text_format_t first=source[format_at(source,count,begin)];first.offset=destination;out[(*used)++]=first;
  for(unsigned i=0;i<count;i++)if(source[i].offset>begin&&source[i].offset<end) {
    out[*used]=source[i];out[(*used)++].offset=destination+source[i].offset-begin;
  }
}
bool dg_text_format_splice(dg_runtime_t *d,uint32_t member,const char *kind,lv_t first,lv_t last,
                           lv_t replacement,lv_t newfulltext) {
  const dg_member_t *m=dg_member(d,member);if(!m||(m->type!=3&&m->type!=7))return false;
  lv_runtime_t *r=d->values;int fi=field(d,member);if(fi<0)return true;
  lv_t old_value=r->roots[DG_FIELD_TEXT_ROOT+fi];
  const char *old=lv_cstr(r,old_value),*next=lv_cstr(r,newfulltext);
  size_t old_bytes=strlen(old),new_bytes=strlen(next),a=0,b=0;
  unsigned old_extent=format_units(old,old_bytes),new_extent=format_units(next,new_bytes);
  if(eq(kind,"char")) {
    int lo=lv_integer(r,first),hi=lv_integer(r,last);
    if(lo==-30000)lo=hi=(int)old_extent;
    if(lo<1){a=0;b=old_bytes;}else {if(hi<1)hi=lo;a=format_byte(old,(unsigned)lo-1);b=format_byte(old,(unsigned)hi);if(b<a)b=a;}
  } else (void)lv_chunk_range(r,kind,old_value,first,last,&a,&b);
  unsigned start=format_units(old,a),end=format_units(old,b);
  if(lv_type(replacement)==LV_VOID) {
    // Deleting a word includes its following whitespace; item/line deletion
    // may include the preceding delimiter. Derive that extent from the VM's
    // actual result, anchored at the requested range.
    size_t prefix=0;while(prefix<a&&prefix<new_bytes&&old[prefix]==next[prefix])prefix++;
    if(prefix<a){a=prefix;start=format_units(old,a);}
    end=start+(old_extent>=new_extent?old_extent-new_extent:0);
  }
  if(end>old_extent||new_extent+end<old_extent+start){error(d,"invalid text formatting splice extent",m->name);return true;}
  unsigned inserted=new_extent-(old_extent-(end-start));
  static dg_text_format_t source[1026],out[1030];
  unsigned count=format_collect(d,m,(unsigned)fi,source);if(!count)return true;
  // Retain unchanged characters inside replacement chunks too: a put-after
  // expression hands the old chunk plus its appended text to the VM.
  size_t old_start=format_byte(old,start),old_end=format_byte(old,end);
  size_t new_start=format_byte(next,start),new_end=format_byte(next,start+inserted);
  size_t prefix=0,old_span=old_end-old_start,new_span=new_end-new_start;
  while(prefix<old_span&&prefix<new_span&&old[old_start+prefix]==next[new_start+prefix])prefix++;
  while(prefix&&prefix<old_span&&((unsigned char)old[old_start+prefix]&0xc0)==0x80)prefix--;
  size_t suffix=0;
  while(suffix<old_span-prefix&&suffix<new_span-prefix&&old[old_end-suffix-1]==next[new_end-suffix-1])suffix++;
  while(suffix&&(((unsigned char)old[old_end-suffix]&0xc0)==0x80||((unsigned char)next[new_end-suffix]&0xc0)==0x80))suffix--;
  unsigned kept_prefix=format_units(old+old_start,prefix),kept_suffix=format_units(old+old_end-suffix,suffix),used=0;
  format_piece(source,count,0,start+kept_prefix,0,out,&used);
  unsigned middle=start+kept_prefix,middle_end=start+inserted-kept_suffix;
  if(middle<middle_end) {
    unsigned inherit=start+kept_prefix;if(kept_prefix)inherit--;
    out[used]=source[format_at(source,count,inherit)];out[used++].offset=middle;
  }
  format_piece(source,count,end-kept_suffix,old_extent,start+inserted-kept_suffix,out,&used);
  // The end record carries the member's insertion style independently from
  // the final visible character.
  out[used]=source[format_at(source,count,old_extent)];out[used++].offset=new_extent;
  if(!used||out[0].offset) {
    memmove(out+1,out,used*sizeof(*out));out[0]=format_base(d,m,(unsigned)fi);out[0].offset=0;used++;
  }
  unsigned kept=0;
  for(unsigned i=0;i<used;i++) {
    if(kept&&out[kept-1].offset==out[i].offset){out[kept-1]=out[i];continue;}
    if(!kept||!format_same(&out[kept-1],&out[i]))out[kept++]=out[i];
  }
  unsigned previous=0;(void)format_pool(d,member,&previous);
  if(d->text_format_count-previous+kept>1024){error(d,"mutable text formatting capacity","over 1024 runs");return true;}
  format_clear(d,member);
  for(unsigned i=0;i<kept;i++) {
    unsigned at=d->text_format_count++;d->text_format_members[at]=member;d->text_format_runs[at]=out[i];
  }
  r->roots[DG_FIELD_TEXT_ROOT+fi]=newfulltext;d->field_text_changed[fi]=true;d->visual_revision++;
  return true;
}
