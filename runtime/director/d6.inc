// Extended Director 6 mutable text casts. Save only native field records;
// archive bytes never contain overlay pointers or executable Director code.
static unsigned d6_u16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static void d6_put16(unsigned char *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static dg_member_t *d6_new_member(dg_runtime_t *d, unsigned file, unsigned cast,
                                 unsigned number, unsigned type) {
  const dg_movie_t *movie = dg_loaded(d, file);
  unsigned slot = 0;
  while (slot < d->dynamic_count && d->dynamic_members[slot].member.type) slot++;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  bool bitmap = type == 1;
#else
  bool bitmap = false;
#endif
  if (!movie || (type != 3 && type != 7 && !bitmap) || slot == 32) {
    error(d, "dynamic text cast capacity/type", "unsupported"); return NULL;
  }
  if (!number) {
    for (unsigned i = 0; i < movie->member_count; i++)
      if (movie->members[i].cast == cast && number < movie->members[i].number)
        number = movie->members[i].number;
    for (unsigned i = 0; i < d->dynamic_count; i++) {
      const dg_member_t *m = &d->dynamic_members[i].member;
      if (DG_ID_FILE(m->id) == file && m->cast == cast && number < m->number) number = m->number;
    }
    number++;
  }
  if (number > 65535 || dg_member(d, DG_ID(file, cast, number))) {
    error(d, "dynamic member number", "invalid"); return NULL;
  }
  unsigned index = slot;
  if (slot == d->dynamic_count) d->dynamic_count++;
  d->dynamic_members[index].name[0] = 0;
  dg_member_t *m = &d->dynamic_members[index].member;
  *m = (dg_member_t){.id=DG_ID(file, cast, number), .number=number,
    .cast=cast, .type=type, .name=d->dynamic_members[index].name, .text="", .asset=""};
  return m;
}
// Mutable field indices are internal, so removal can compact every parallel
// array without changing a cast-member identity held by recovered scripts.
static void d6_erase_field(dg_runtime_t *d, uint32_t id) {
#if DG_MODERN
  button_forget(d,id);
#endif
  format_clear(d,id);
  for (unsigned i = 0; i < d->field_count; i++) if (d->field_members[i] == id) {
    unsigned last = --d->field_count;
    d->field_members[i] = d->field_members[last];
    d->values->roots[DG_FIELD_TEXT_ROOT + i] = d->values->roots[DG_FIELD_TEXT_ROOT + last];
    d->values->roots[DG_FIELD_COLOR_ROOT + i] = d->values->roots[DG_FIELD_COLOR_ROOT + last];
    memcpy(d->field_fonts[i], d->field_fonts[last], sizeof(d->field_fonts[i]));
    d->field_text_changed[i] = d->field_text_changed[last];
    d->field_scroll[i] = d->field_scroll[last];
    d->field_font_changed[i] = d->field_font_changed[last];
    d->field_sizes[i] = d->field_sizes[last];
    d->field_align[i] = d->field_align[last];
    d->field_faces[i] = d->field_faces[last];
    d->field_face_changed[i] = d->field_face_changed[last];
    d->field_size_changed[i] = d->field_size_changed[last];
    d->field_line_space[i]=d->field_line_space[last];d->field_char_spacing[i]=d->field_char_spacing[last];
    d->field_spacing_changed[i]=d->field_spacing_changed[last];
#if DG_CAP_TEXT_BOX
    d->field_box_live[i]=d->field_box_live[last];d->field_box_types[i]=d->field_box_types[last];
    d->field_margins[i]=d->field_margins[last];d->field_borders[i]=d->field_borders[last];
    d->field_box_shadows[i]=d->field_box_shadows[last];d->field_word_wrap[i]=d->field_word_wrap[last];
    d->field_box_bg[i]=d->field_box_bg[last];d->field_box_paint[i]=d->field_box_paint[last];
    d->field_widths[i]=d->field_widths[last];d->field_heights[i]=d->field_heights[last];
    d->field_content_heights[i]=d->field_content_heights[last];
    d->field_measure_revision[i]=d->field_measure_revision[last];
#endif
    d->field_editable[i] = d->field_editable[last];
    d->field_members[last] = 0;
    d->values->roots[DG_FIELD_TEXT_ROOT + last] = (lv_t){0};
    d->values->roots[DG_FIELD_COLOR_ROOT + last] = (lv_t){0};
    memset(d->field_fonts[last], 0, sizeof(d->field_fonts[last]));
    d->field_text_changed[last] = d->field_editable[last] = false;
    d->field_scroll[last] = 0;
    d->field_font_changed[last] = false;
    d->field_sizes[last] = d->field_align[last] = 0;
    return;
  }
}
static bool d6_member_editable(dg_runtime_t *d, const dg_member_t *m) {
  if (!m || (m->type != 3 && m->type != 7)) return false;
  for (unsigned i = 0; i < d->field_count; i++)
    if (d->field_members[i] == m->id) return d->field_editable[i];
  return m->editable;
}
bool dg_text_editable(dg_runtime_t *d, unsigned sprite) {
  if (!sprite || sprite >= DG_SPRITES) return false;
  const dg_member_t *m = dg_member(d, d->sprites[sprite].value.member);
  if (!m || (m->type != 3 && m->type != 7)) return false;
  if(dg_button_kind(m))return false;
  return d->sprite_editable[sprite] ? d->sprite_editable[sprite] > 0
      : (d->sprites[sprite].value.flags & 64) || d6_member_editable(d, m);
}
static const char *d6_text(dg_runtime_t *d, const dg_member_t *m) {
  if (!m) return "";
  for (unsigned i=0;i<d->field_count;i++)
    if (d->field_members[i]==m->id) return lv_cstr(d->values,d->values->roots[DG_FIELD_TEXT_ROOT+i]);
  return m->text ? m->text : "";
}
static float d6_kerning(const dg_text_style_t *style, unsigned first, unsigned second) {
  for (unsigned i=0;i<style->kerning_count;i++) {
    const int16_t *pair=style->kerning+i*3;
    if ((unsigned)pair[0]==first && (unsigned)pair[1]==second)
      return pair[2]*style->size/127.0f;
  }
  return 0;
}
static lv_t d6_char_position(dg_runtime_t *d, const dg_member_t *m, int position) {
  if(m && d->platform.text_position) {
    int x=0,y=0;
    if(d->platform.text_position(d->context,m->id,position,&x,&y))return lv_point(d->values,x,y);
    if(d->values->failed)return (lv_t){0};
  }
#if DIRECTOR64_DIRECTOR_VERSION >= 10
  // A member whose converted style carries no advance table still answers a
  // position: the exercises place highlight boxes from it, and half an em
  // per character is the usual average for this Latin text. Approximate,
  // and traced so the evidence shows which screens rely on it.
  if (m && m->text_style && !m->text_style->advances) {
    const dg_text_style_t *style = m->text_style;
    const unsigned char *text = (const unsigned char *)d6_text(d, m);
    unsigned line = 0, column = 0;
    for (int i = 0; text[i] && i < position - 1; i++) {
      if (text[i] == '\r' || text[i] == '\n') { column = 0; line++; continue; }
      column++;
    }
    if (d->platform.trace) {
      char note[96];
      snprintf(note, sizeof(note), "TEXT_METRICS_PENDING member=%lu",
               (unsigned long)m->id);
      d->platform.trace(d->context, note);
    }
    return lv_point(d->values, (int)(column * style->size / 2),
                    line * style->line_height);
  }
#endif
  if (!m || !m->text_style || !m->text_style->advances) {
    error(d,"text character metrics","unavailable");return (lv_t){0};
  }
  const dg_text_style_t *style=m->text_style;
#if DIRECTOR64_DIRECTOR_VERSION >= 10
  // The measured D10 tables cover windows-1252 text stored as UTF-8; walk
  // codepoints, with the recorded half-em fallback outside the table.
  {
    const char *text = d6_text(d, m);
    float x = 0;
    unsigned line = 0, last = 0;
    for (int i = 0; *text && i < position - 1; i++) {
      unsigned cp = dg_utf8_next(&text);
      if (cp == '\r' || cp == '\n') {
        x = 0;
        if (cp == '\r' || !i || last != '\r') line++;
        last = cp;
        continue;
      }
      last = cp;
      x += dg_style_advance(style, cp);
      if (cp >= 32 && cp <= 126) {
        const char *peek = text;
        unsigned next_cp = *peek ? dg_utf8_next(&peek) : 0;
        if (next_cp >= 32 && next_cp <= 126)
          x += d6_kerning(style, cp, next_cp);
      }
    }
    return lv_point(d->values, (int)x, line * style->line_height);
  }
#else
  const unsigned char *text=(const unsigned char *)d6_text(d,m);
  float x=0;unsigned line=0;
  // nthCharacter is one-based; a position past the last character is the caret.
  for (int i=0;*text && i<position-1;i++,text++) {
    if (*text=='\r' || *text=='\n') {
      x=0;
      if (*text=='\r' || !i || text[-1]!='\r') line++;
      continue;
    }
    if (*text<32 || *text>126) {error(d,"controller text metrics","non-ASCII character");break;}
    x+=style->advances[*text-32]+d6_kerning(style,*text,text[1]);
  }
  return lv_point(d->values,(int)x,line*style->line_height);
#endif
}
static bool d6_get(dg_runtime_t *d, const char *name, lv_t owner, lv_t *out) {
  unsigned ln = lv_name_id(d->values, name);
  if (lv_type(owner) == LV_VOID) {
    if ((ln == LN_RIGHTMOUSEDOWN) || (ln == LN_RIGHTMOUSEUP)) {
      *out = lv_num((ln == LN_RIGHTMOUSEDOWN) ? d->right_mouse_down : !d->right_mouse_down);
      return true;
    }
    if (ln == LN_RUNMODE) {
      *out = lv_text(d->values, "Projector", false); return true;
    }
    if ((ln == LN_OPTIONDOWN) || (ln == LN_CONTROLDOWN) || (ln == LN_COMMANDDOWN) || (ln == LN_SHIFTDOWN)) {
#if DG_CAP_KEYBOARD
      dg_runtime_t *keyboard=d;
#if DG_D10
      keyboard=sound_host(d); // Physical modifiers are shared by the windows.
#endif
      unsigned key=ln==LN_OPTIONDOWN?58:ln==LN_SHIFTDOWN?56:59;
      bool held=(keyboard->keys_down[key>>5]&(UINT32_C(1)<<(key&31)))!=0;
      unsigned right=ln==LN_OPTIONDOWN?61:ln==LN_SHIFTDOWN?60:62;
      held|=(keyboard->keys_down[right>>5]&(UINT32_C(1)<<(right&31)))!=0;
      // Windows projectors expose Control through commandDown as well.
      if(ln==LN_COMMANDDOWN)held|=(keyboard->keys_down[55>>5]&(UINT32_C(1)<<(55&31)))!=0 ||
          (keyboard->keys_down[54>>5]&(UINT32_C(1)<<(54&31)))!=0;
      *out=lv_num(held);return true;
#else
      *out = lv_num(0); return true;
#endif
    }
#if DIRECTOR64_DIRECTOR_VERSION >= 10
    if (ln == LN_KEYBOARDFOCUSSPRITE) {
      // Director answers -1 when no sprite holds the keyboard.
      *out = lv_num(d->keyboard_focus ? (double)d->keyboard_focus : -1);
      return true;
    }
#endif
    if ((ln == LN_SELSTART) || (ln == LN_SELEND)) {
      *out = lv_num((ln == LN_SELSTART) ? d->sel_start : d->sel_end); return true;
    }
#if DG_CAP_KEYBOARD && DG_D10
    // A typed key, not a text-entry replay: the keyboard's own values.
    if (((ln == LN_KEY) || (ln == LN_KEYCODE)) && !d->text_key) return false;
#endif
    if (ln == LN_KEY) {
      char key[2] = {(char)d->text_key, 0};
      *out = lv_text(d->values, key, false); return true;
    }
    if (ln == LN_KEYCODE) { *out = lv_num(d->text_key_code); return true; }
  }
  if ((lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_FIELD) && (ln == LN_LINECOUNT)) {
    const char *text=d6_text(d,dg_member(d,(uint32_t)lv_id(owner)));
    unsigned lines=1;char previous=0;
    for (;*text;text++) {
      if (*text=='\r' || (*text=='\n' && previous!='\r')) lines++;
      previous=*text;
    }
    *out=lv_num(lines);return true;
  }
  if ((lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_FIELD) && (ln == LN_EDITABLE)) {
    *out = lv_num(d6_member_editable(d, dg_member(d, (uint32_t)lv_id(owner)))); return true;
  }
  if (lv_type(owner) == LV_SPRITE && (ln == LN_EDITABLETEXT)) {
    *out = lv_num(dg_text_editable(d, (unsigned)lv_id(owner))); return true;
  }
  if ((lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_FIELD) && (ln == LN_MEDIA)) {
    *out = owner; return true;
  }
  if (lv_type(owner) == LV_CASTLIB && (ln == LN_NUMBEROFMEMBERS)) {
    unsigned count = 0;
    if (lv_id(owner) < 1 || (unsigned)lv_id(owner) > d->movie->cast_count) return false;
    const dg_cast_t *cast = &d->movie->casts[lv_id(owner) - 1];
    const dg_movie_t *movie = dg_loaded(d, cast->file);
    if (movie) for (unsigned i = 0; i < movie->member_count; i++)
      if (movie->members[i].cast == cast->cast && movie->members[i].number > count) count = movie->members[i].number;
    for (unsigned i = 0; i < d->dynamic_count; i++) {
      const dg_member_t *m = &d->dynamic_members[i].member;
      if (DG_ID_FILE(m->id) == cast->file && m->cast == cast->cast && m->number > count) count = m->number;
    }
    *out = lv_num(count); return true;
  }
  return false;
}
static bool d6_set(dg_runtime_t *d, const char *name, lv_t owner, lv_t value) {
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
#if DIRECTOR64_DIRECTOR_VERSION >= 10
  // Keyboard focus follows the editable field the exercises activate; the
  // controller keyboard commits into whichever sprite holds it.
  if (lv_type(owner) == LV_VOID && (ln == LN_KEYBOARDFOCUSSPRITE)) {
    int n = lv_integer(r, value);
    d->keyboard_focus = n > 0 && n < DG_SPRITES ? (unsigned)n : 0;
    return true;
  }
#endif
  if (lv_type(owner) == LV_VOID && ((ln == LN_SELSTART) || (ln == LN_SELEND))) {
    int n = lv_integer(r, value);
    if (ln == LN_SELSTART) d->sel_start = n < 0 ? 0 : (unsigned)n;
    else d->sel_end = n < 0 ? 0 : (unsigned)n;
    return true;
  }
  if (lv_type(owner) == LV_SPRITE && (ln == LN_EDITABLETEXT)) {
    if (lv_id(owner) < 1 || lv_id(owner) >= DG_SPRITES) error(d, "editable sprite", "range");
    else d->sprite_editable[lv_id(owner)] = lv_truth(r, value) ? 1 : -1;
    return true;
  }
  if (lv_type(owner) != LV_MEMBER && lv_type(owner) != LV_FIELD) return false;
  if (ln == LN_EDITABLE) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (!m || (m->type != 3 && m->type != 7)) error(d, "editable member", "not text");
    else { int slot = field(d, m->id); if (slot >= 0) d->field_editable[slot] = lv_truth(r, value); }
    return true;
  }
  if (ln == LN_NAME) {
    for (unsigned i = 0; i < d->dynamic_count; i++) if (d->dynamic_members[i].member.id == (uint32_t)lv_id(owner)) {
      const char *text = lv_cstr(r, value);
      // A name the port cannot hold stops the handler and leaves the score
      // running, the way the projector answers a script it cannot complete.
      // Willy reaches this by its own doing: it stores a player under
      // "User" & id & "DB", so a player called S collides with UsersDB, the
      // member holding its user list, and the junk proplist lands there
      // instead. The next login then asks to name a member after 436
      // characters of car parts. Halting the movie there costs the player
      // every other save; an alert costs them the one bad name.
      if (!*text || strlen(text) >= sizeof(d->dynamic_members[i].name))
        script_error(d, "dynamic member name", "too long for this port");
      else strcpy(d->dynamic_members[i].name, text);
      return true;
    }
  }
  if (ln == LN_MEDIA) {
    const dg_member_t *source = dg_member(d, (uint32_t)lv_id(value)), *target = dg_member(d, (uint32_t)lv_id(owner));
    if (!source || !target || (source->type != 3 && source->type != 7) || (target->type != 3 && target->type != 7)) {
      error(d, "copy member media", "only text casts supported"); return true;
    }
    lv_t text = lv_text(r, source->text, false);
    for (unsigned i = 0; i < d->field_count; i++)
      if (d->field_members[i] == source->id) text = r->roots[DG_FIELD_TEXT_ROOT + i];
    int slot = field(d, target->id);
    if (slot >= 0) r->roots[DG_FIELD_TEXT_ROOT + slot] = text;
    return true;
  }
  return false;
}
static unsigned d6_cast_capacity(void) {
#if defined(DIRECTOR64_SAVE_SLOT_BYTES) && DIRECTOR64_SAVE_SLOT_BYTES > 65536
  return DIRECTOR64_SAVE_SLOT_BYTES-512u;
#else
  return 65000;
#endif
}
static bool d6_save_cast(dg_runtime_t *d, const dg_cast_t *cast) {
  const dg_movie_t *movie = dg_loaded(d, cast->file);
  if (!movie || !d->platform.write_file) return false;
  unsigned capacity=d6_cast_capacity();
  unsigned char *data = malloc(capacity);
  if (!data) return false;
  memcpy(data, "D6TF\1\0", 6);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  if(cast_images_count(d,cast))data[4]=2;
#endif
  unsigned at = 8, count = 0;
  for (unsigned i = 0; i < d->field_count; i++) {
    const dg_member_t *m = dg_member(d, d->field_members[i]);
    if (!m || DG_ID_FILE(m->id) != cast->file || m->cast != cast->cast || !m->type) continue;
    const char *text = lv_cstr(d->values, d->values->roots[DG_FIELD_TEXT_ROOT + i]);
    unsigned n = (unsigned)strlen(m->name), length = (unsigned)strlen(text);
    if (!n || n > 63 || length > 16383 || at + 8 + n + length > capacity) { free(data); return false; }
    d6_put16(data + at, m->number); d6_put16(data + at + 2, m->type);
    d6_put16(data + at + 4, n); d6_put16(data + at + 6, length); at += 8;
    memcpy(data + at, m->name, n); at += n;
    memcpy(data + at, text, length); at += length; count++;
  }
  d6_put16(data + 6, count);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  if(!cast_images_write(d,cast,data,capacity,&at)){free(data);return false;}
#endif
  const char *store_name = movie->code->name;
#if DIRECTOR64_DIRECTOR_VERSION >= 10
  const char *store = d10_cast_store(d, cast->file);
  if (store) store_name = store;
#endif
  bool ok = d->platform.write_file(d->context, store_name, (const char *)data, at);
  free(data); return ok;
}
static void d6_restore_cast_file(dg_runtime_t *d, const dg_cast_t *cast,
                                 const char *filename) {
  if (!d->platform.read_file) return;
  {
    unsigned capacity=d6_cast_capacity();
    unsigned char *data = malloc(capacity+1u); unsigned length = 0;
    if (!data) { error(d,"cast restore memory","allocation"); return; }
    if (!d->platform.read_file(d->context, filename, (char *)data, capacity+1u, &length)) {free(data);return;}
    unsigned at = 8, count = length >= 8 ? d6_u16(data + 6) : 0;
    bool images=false;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    images=length>=8&&!memcmp(data,"D6TF\2\0",6);
#endif
    bool valid = length >= 8 && length <= capacity && (images||!memcmp(data,"D6TF\1\0",6)) && count <= DG_FIELDS;
    unsigned numbers[DG_FIELDS], names[DG_FIELDS], name_sizes[DG_FIELDS];
    unsigned new_members = 0, new_fields = 0;
    for (unsigned pass = 0; pass < 2 && valid; pass++) {
      at = 8;
      for (unsigned i = 0; i < count && valid; i++) {
        if (at + 8 > length) { valid = false; break; }
        unsigned number=d6_u16(data+at),type=d6_u16(data+at+2),n=d6_u16(data+at+4),size=d6_u16(data+at+6); at+=8;
        if (!number || (type != 3 && type != 7) || !n || n > 63 || size > 16383 || at+n+size > length || memchr(data+at,0,n+size)) {valid=false;break;}
        uint32_t id = DG_ID(cast->file, cast->cast, number);
        const dg_member_t *m = dg_member(d,id);
        if (!pass) {
          for (unsigned j = 0; j < i; j++)
            if (numbers[j] == number || (name_sizes[j] == n && !memcmp(data + names[j], data + at, n))) valid = false;
          numbers[i] = number; names[i] = at; name_sizes[i] = n;
          if (m && (strlen(m->name) != n || memcmp(m->name, data + at, n) || m->type != type)) valid = false;
          new_members += !m;
          bool cached = false;
          for (unsigned j = 0; j < d->field_count; j++) cached |= d->field_members[j] == id;
          new_fields += !cached;
        } else {
          if (!m) {
            dg_member_t *created = d6_new_member(d,cast->file,cast->cast,number,type);
            if (!created) {valid=false;break;}
            char *name=(char *)created->name;
            memcpy(name,data+at,n);name[n]=0;m=created;
          }
          if (strlen(m->name)!=n || memcmp(m->name,data+at,n) || m->type!=type) {valid=false;break;}
          int slot=field(d,id); char *text=malloc(size+1);
          if (slot<0 || !text) {free(text);valid=false;break;}
          memcpy(text,data+at+n,size);text[size]=0;
          d->values->roots[DG_FIELD_TEXT_ROOT+slot]=lv_text(d->values,text,false);free(text);
        }
        at+=n+size;
      }
      if(images) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
        valid &= cast_images_restore(d,cast,data,length,at,new_members,pass!=0);
#endif
      } else valid &= at == length;
      if (!pass) valid &= new_members + d->dynamic_count <= 32 && new_fields + d->field_count <= DG_FIELDS;
    }
    free(data);
    if (!valid) error(d,"native cast save","invalid records");
  }
}
static void d6_restore_casts(dg_runtime_t *d) {
  for (unsigned c = 0; c < d->movie->cast_count && !d->values->failed; c++) {
    const dg_cast_t *cast = &d->movie->casts[c];
    if (cast->file >= 64 || d->restored_casts[cast->file]) continue;
    d->restored_casts[cast->file] = true;
    const dg_movie_t *movie = dg_loaded(d, cast->file);
    if (!movie) continue;
#if DIRECTOR64_DIRECTOR_VERSION >= 10
    // A storage-retargeted cast reloads from its selected save file.
    const char *store = d10_cast_store(d, cast->file);
    d6_restore_cast_file(d, cast, store ? store : movie->code->name);
#else
    d6_restore_cast_file(d, cast, movie->code->name);
#endif
  }
}
static bool d6_call(dg_runtime_t *d, const char *name, unsigned argc, const lv_t *a, lv_t *out) {
  unsigned ln = lv_name_id(d->values, name);
  if ((ln == LN_CERTIFICATE_PRINT_NOTICE) && !argc) {
    snprintf(d->notice, sizeof(d->notice),
             "Drucken ist hier nicht verfuegbar.\nDeine Urkunde bleibt hier sichtbar.");
    d->notice_armed = false;
    if (d->platform.trace) d->platform.trace(d->context, "CERTIFICATE_PRINT_NOTICE");
    return true;
  }
  if (ln == LN_PUPPETTRANSITION)
    return puppet_transition(d, argc, a);

  lv_runtime_t *r=d->values;
  if (ln == LN_UNLOADMEMBER) {
    // This is a purge hint, not erase: immutable package assets are demand-loaded
    // and live script/text references own their lifetime independently.
    if (argc>2) error(d,"unloadMember arguments","range");
    return true;
  }
  if ((ln == LN_CHARPOSTOLOC) && argc==2) {
    *out=d6_char_position(d,dg_member(d,(uint32_t)lv_id(a[0])),lv_integer(r,a[1]));return true;
  }
  // Director suppresses score navigation while outgoing sprite callbacks run.
  if ((ln == LN_GO) && d->ending_sprites) return true;
  if ((ln == LN_QUIT) && !argc) { d->quit_requested = true; return true; }
  if ((ln == LN_PASS) && !argc) { d->pass_event = true; return true; }
  if ((ln == LN_DONTPASSEVENT) && !argc) { d->pass_event = false; return true; }
  // mouseH() and mouseV() read like their the-properties (Adventskalender).
  if (((ln == LN_MOUSEH) || (ln == LN_MOUSEV)) && !argc) {
    *out = lv_num(ln == LN_MOUSEH ? d->mouse_x : d->mouse_y);
    return true;
  }
  if ((ln == LN_NEW) && argc==2 && lv_type(a[1])==LV_CASTLIB) {
    unsigned type=eq(lv_cstr(r,a[0]),"field")?7:eq(lv_cstr(r,a[0]),"text")?3:0;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    // A font member stands in as a text member: Löwenzahn 6's paint box
    // makes one only to ask its fontList, then erases it.
    if (eq(lv_cstr(r,a[0]),"font")) type=3;
    // A bitmap holds a stage snapshot (Birthday freezes the room behind its
    // popups): its image comes from an image assignment (d10.inc).
    if (eq(lv_cstr(r,a[0]),"bitmap")) type=1;
#endif
#if DIRECTOR64_DIRECTOR_VERSION >= 10
    const dg_cast_t *cast=d10_castlib(d,lv_id(a[1]));
    if (!cast) {error(d,"new cast library","invalid");return true;}
#else
    if (lv_id(a[1])<1 || (unsigned)lv_id(a[1])>d->movie->cast_count) {error(d,"new cast library","invalid");return true;}
    const dg_cast_t *cast=&d->movie->casts[lv_id(a[1])-1];
#endif
    dg_member_t *m=d6_new_member(d,cast->file,cast->cast,0,type);
    if(m)*out=lv_make(LV_MEMBER, (int32_t)m->id);
    return true;
  }
  if ((ln == LN_SAVE) && argc==1 && lv_type(a[0])==LV_CASTLIB) {
#if DIRECTOR64_DIRECTOR_VERSION >= 10
    const dg_cast_t *save_lib=d10_castlib(d,lv_id(a[0]));
    bool ok=save_lib && d6_save_cast(d,save_lib);
#else
    bool ok=lv_id(a[0])>=1 && (unsigned)lv_id(a[0])<=d->movie->cast_count && d6_save_cast(d,&d->movie->casts[lv_id(a[0])-1]);
#endif
    *out=lv_num(ok);
    if(!ok)error(d,"cast save","failed");
    return true;
  }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // The system's installed fonts: none beyond the converted ones, so a
  // script picking a system font takes its own fallback.
  if ((ln == LN_FONTLIST) && argc == 1 && lv_type(a[0]) == LV_MEMBER) {
    *out = lv_list(r, 0, NULL, false);
    return true;
  }
#endif
  if ((ln == LN_ERASE) && argc == 1 && lv_type(a[0]) == LV_MEMBER) {
    for (unsigned i = 0; i < d->dynamic_count; i++)
      if (d->dynamic_members[i].member.id == (uint32_t)lv_id(a[0])) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
        cast_images_erase(d,(uint32_t)lv_id(a[0]));
#endif
        d6_erase_field(d, (uint32_t)lv_id(a[0]));
        d->dynamic_members[i].member = (dg_member_t){0};
        d->dynamic_members[i].name[0] = 0;
        *out = lv_num(1); return true;
      }
#if DIRECTOR64_DIRECTOR_VERSION >= 10
    // Sweeping erase loops cover freed member numbers; an absent member
    // erases to nothing exactly as the original.
    if (!dg_member(d, (uint32_t)lv_id(a[0]))) { *out = lv_num(1); return true; }
#endif
    error(d, "erase cast member", "only native saved text members are mutable");return true;
  }
  if (ln == LN_TELL_WINDOW) {error(d,"external file chooser window","unsupported");return true;}
  if (ln == LN_OPEN_WINDOW_TRAP) {
    // The desktop file-exchange chooser cannot open on the target. Raise the
    // recoverable alert before the original handler would deactivate the
    // save screen, so the screen stays usable after the click.
    script_error(d,"external file chooser window","unsupported");return true;
  }
  return false;
}
