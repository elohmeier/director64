// Original8.5 distinguishes intrinsic text bounds from the displayed page:
// fixed/scroll retain pageHeight, while adjust follows wrapped content height.
// Host line placement is shared by visible glyphs, images and text hit testing.
#include "buttons.h"
bool dg_text_box_settings(dg_runtime_t *d, uint32_t id,
                          dg_text_box_settings_t *out) {
  const dg_member_t *m = dg_member(d, id);
  if (!m || (m->type != 3 && m->type != 7))
    return false;
  *out = (dg_text_box_settings_t){
      .margin = m->text_box_present ? m->text_margin : 0,
      .border = m->text_box_present ? m->text_border : 0,
      .shadow = m->text_box_present ? m->text_box_shadow : 0,
      .bg_rgb = m->text_box_present ? m->text_box_bg : 0xffffff,
      .present = m->text_box_present,
      .wrap = !m->text_box_present || m->text_word_wrap};
  for (unsigned i = 0; i < d->field_count; i++)
    if (d->field_members[i] == id) {
      out->margin = d->field_margins[i];
      out->border = d->field_borders[i];
      out->shadow = d->field_box_shadows[i];
      out->wrap = d->field_word_wrap[i];
      out->bg_rgb = d->field_box_bg[i];
      out->present = d->field_box_paint[i];
      break;
    }
  return true;
}
static bool text_box_property(dg_runtime_t *d, unsigned ln, uint32_t id,
                              lv_t *out) {
  dg_text_box_settings_t settings;
  if (ln != LN_MARGIN && ln != LN_BORDER && ln != LN_BOXDROPSHADOW &&
      ln != LN_WORDWRAP && ln != LN_BGCOLOR)
    return false;
  if (!dg_text_box_settings(d, id, &settings))
    return false;
  if (ln == LN_BGCOLOR) {
    *out = lv_make(LV_COLOR, settings.bg_rgb);
    return true;
  }
  *out = lv_num(ln == LN_MARGIN          ? settings.margin
                : ln == LN_BORDER        ? settings.border
                : ln == LN_BOXDROPSHADOW ? settings.shadow
                                         : settings.wrap);
  return true;
}
static int text_mouse_line(dg_runtime_t *d) {
  unsigned channel = dg_hit(d, d->mouse_x, d->mouse_y);
  if (!channel || channel >= DG_SPRITES)
    return -1;
  const dg_member_t *m = dg_member(d, d->sprites[channel].value.member);
  if (!m || (m->type != 3 && m->type != 7))
    return -1;
  int l, t, r, b;
  dg_sprite_bounds(d, &d->sprites[channel], &l, &t, &r, &b);
  int x = d->mouse_x - l, y = d->mouse_y - t, width = r - l;
  if (width <= 0 || b <= t || x < 0 || x >= width || y < 0 || y >= b - t)
    return -1;
  lv_t text = lv_get(d->values, NULL, "text", lv_make(LV_MEMBER, m->id));
  unsigned lines = lv_chunk_count(d->values, "line", text);
  if (!lines)
    return -1;
  int index = -1;
  if (d->platform.text_char_at) {
    index = d->platform.text_char_at(d->context, m->id, channel, x, y);
    // A line remains selectable to the right of its final glyph. Query its
    // first occupied cell instead of requiring ink under the pointer.
    if (index < 0)
      index = d->platform.text_char_at(d->context, m->id, channel, 0, y);
  }
  if (index >= 0) {
    unsigned line = 1;
    if (index) {
      lv_t prefix = lv_chunk(d->values, "char", text, lv_num(1), lv_num(index));
      const char *s = lv_cstr(d->values, prefix);
      for (unsigned i = 0; i < lv_byte_count(d->values, prefix); i++)
        if (s[i] == '\r')
          line++;
    }
    return line;
  }
  int pitch = lv_integer(d->values, lv_get(d->values, NULL, "lineHeight",
                                           lv_make(LV_MEMBER, m->id)));
  int scroll = 0;
  for (unsigned i = 0; i < d->field_count; i++)
    if (d->field_members[i] == m->id)
      scroll = d->field_scroll[i];
  unsigned line =
      pitch > 0 ? (unsigned)((y + (scroll > 0 ? scroll : 0)) / pitch) + 1 : 0;
  return line && line <= lines ? (int)line : -1;
}
bool dg_text_dimensions(dg_runtime_t *d, uint32_t id, bool content,
                        unsigned *width, unsigned *height) {
  const dg_member_t *m = dg_member(d, id);
  if (!m || (m->type != 3 && m->type != 7))
    return false;
  if(dg_button_kind(m))return false;
  for (unsigned i = 0; i < d->field_count; i++)
    if (d->field_members[i] == id && d->field_box_live[i]) {
      *width = d->field_widths[i];
      if(!content&&d->field_box_types[i]!=1){*height=d->field_heights[i];return true;}
      if (*width && d->field_measure_revision[i] != d->visual_revision + 1) {
        int measured = d->platform.text_height
                           ? d->platform.text_height(d->context, id, *width)
                           : -1;
        if (measured < 1 || measured > 4096) {
          error(
              d,
              "text box line measurement unavailable or outside 1..4096 pixels",
              m->name);
          return true;
        }
        d->field_content_heights[i] = (uint16_t)measured;
        d->field_measure_revision[i] = d->visual_revision + 1;
      }
      *height = content || d->field_box_types[i] == 1
                    ? d->field_content_heights[i]
                    : d->field_heights[i];
      return true;
    }
  *width = m->width;
  *height = m->height;
  if(m->text_box_present && *width && (content||m->text_box_type==1)) {
    int measured=d->platform.text_height?d->platform.text_height(d->context,id,*width):-1;
    if(measured<1||measured>4096)error(d,"source text box line measurement unavailable or outside 1..4096 pixels",m->name);
    else *height=(unsigned)measured;
  }
  return true;
}
#if DG_EXTENDED
static bool text_box_accepts_key(dg_runtime_t *d,uint32_t id,const char *proposed) {
  const dg_member_t *m=dg_member(d,id);if(!m)return false;
  unsigned kind=m->text_box_present?m->text_box_type:1;
  for(unsigned i=0;i<d->field_count;i++)if(d->field_members[i]==id)kind=d->field_box_types[i];
  if(kind!=4)return true;
  int fi=field(d,id);if(fi<0)return false;
  if(!d->field_widths[fi]||!d->field_heights[fi])return false;
  lv_runtime_t *r=d->values;
  if(lv_type(r->roots[DG_TEXT_PROBE_ROOT])!=LV_VOID){error(d,"recursive limited field measurement",m->name);return false;}
  r->roots[DG_TEXT_PROBE_ROOT]=r->roots[DG_FIELD_TEXT_ROOT+fi];
  lv_t value=lv_text(r,proposed,false);
  bool changed=d->field_text_changed[fi];r->roots[DG_FIELD_TEXT_ROOT+fi]=value;d->field_text_changed[fi]=true;
  unsigned width=d->field_widths[fi],height=d->field_heights[fi];
  int measured=d->platform.text_height?d->platform.text_height(d->context,id,width):-1;
  bool accepted=measured>=1&&(unsigned)measured<=height;
  if(measured<1)error(d,"limited field line measurement unavailable",m->name);
  if(accepted&&!d->field_word_wrap[fi]) {
    int x=0,y=0;unsigned position=lv_chunk_count(r,"char",value)+1;
    if(!d->platform.text_position||!d->platform.text_position(d->context,id,(int)position,&x,&y)) {
      error(d,"limited field caret measurement unavailable",m->name);accepted=false;
    } else accepted=x>=0&&(unsigned)x<=width;
  }
  r->roots[DG_FIELD_TEXT_ROOT+fi]=r->roots[DG_TEXT_PROBE_ROOT];r->roots[DG_TEXT_PROBE_ROOT]=(lv_t){0};d->field_text_changed[fi]=changed;
  return accepted&&!r->failed;
}
#endif
static bool text_box_set(dg_runtime_t *d, unsigned ln, uint32_t id,
                         lv_t value) {
  const dg_member_t *m = dg_member(d, id);
  if (!m || (m->type != 3 && m->type != 7) ||
      (ln != LN_WIDTH && ln != LN_HEIGHT && ln != LN_RECT && ln != LN_BOXTYPE &&
       ln != LN_MARGIN && ln != LN_BORDER && ln != LN_BOXDROPSHADOW &&
       ln != LN_WORDWRAP && ln != LN_BGCOLOR))
    return false;
  int index = field(d, id);
  if (index < 0)
    return true;
  unsigned i = (unsigned)index;
  d->field_box_live[i] = true;
  if (ln == LN_BGCOLOR) {
    uint32_t rgb = (uint32_t)lv_integer(d->values, value);
    const uint32_t *palette = dg_stage_palette(d);
    d->field_box_bg[i] = lv_type(value) == LV_COLOR || rgb > 255 || !palette
                             ? rgb & 0xffffff
                             : palette[rgb];
    d->field_box_paint[i] = true;
    return true;
  }
  if (ln == LN_WORDWRAP) {
    d->field_word_wrap[i] = lv_truth(d->values, value);
    return true;
  }
  if (ln == LN_MARGIN || ln == LN_BORDER || ln == LN_BOXDROPSHADOW) {
    d->field_box_paint[i] = true;
    int n = lv_integer(d->values, value);
    if (n < 0 || n > 255) {
      error(d, "text box decoration outside 0..255", m->name);
      return true;
    }
    if (ln == LN_MARGIN)
      d->field_margins[i] = (uint8_t)n;
    else if (ln == LN_BORDER)
      d->field_borders[i] = (uint8_t)n;
    else
      d->field_box_shadows[i] = (uint8_t)n;
    return true;
  }
  if (ln == LN_BOXTYPE) {
    const char *kind = lv_cstr(d->values, value);
    if (eq(kind, "adjust"))
      d->field_box_types[i] = 1;
    else if (eq(kind, "fixed"))
      d->field_box_types[i] = 2;
    else if (eq(kind, "scroll"))
      d->field_box_types[i] = 3;
    else if (eq(kind, "limit"))
      d->field_box_types[i] = 4;
    else
      error(d, "invalid text boxType", kind);
    return true;
  }
  int width = d->field_widths[i], height = d->field_heights[i];
  if (ln == LN_WIDTH)
    width = lv_integer(d->values, value);
  else if (ln == LN_HEIGHT)
    height = lv_integer(d->values, value);
  else {
    if (lv_type(value) != LV_LIST || lv_count(d->values, value) != 4) {
      error(d, "invalid text rectangle", m->name);
      return true;
    }
    width = lv_integer(d->values, lv_at(d->values, value, 3)) -
            lv_integer(d->values, lv_at(d->values, value, 1));
    height = lv_integer(d->values, lv_at(d->values, value, 4)) -
             lv_integer(d->values, lv_at(d->values, value, 2));
  }
  if (width < 0 || height < 0 || width > 4096 || height > 4096) {
    error(d, "text dimensions outside 0..4096 pixels", m->name);
    return true;
  }
  d->field_widths[i] = (uint16_t)width;
  d->field_heights[i] = (uint16_t)height;
  return true;
}
