#include "director.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void apply(dg_sprite_t *, const dg_spec_t *, unsigned);
static const char *basename_(const char *);
#if DG_D10
static const dg_movie_t *d10_cast_target(dg_runtime_t *, const char *);
static int d10_window_member(dg_runtime_t *, const char *, lv_t, uint32_t *);
static const char *d10_cast_store(dg_runtime_t *, uint32_t);
static const dg_cast_t *d10_castlib(dg_runtime_t *, int32_t);
static int32_t d10_castlib_id(dg_runtime_t *, const char *);
static lv_t behavior_property(dg_runtime_t *, unsigned, const char *);
#endif
_Static_assert(DG_FIELD_COLOR_ROOT + DG_FIELDS <= LV_ROOTS,
               "field text and style must fit in the GC roots");

#if DG_MODERN
static void cancel_fade(dg_runtime_t *d, unsigned channel) {
  if (d->fade_duration[channel])
    d->channel_volume[channel] = d->fade_volume[channel];
  d->fade_duration[channel] = 0;
}
#endif
unsigned dg_opacity(const dg_sprite_t *s) {
  return (s->value.thickness & DG_HAS_BLEND) || s->value.ink == 32
             ? s->value.blend
             : 100;
}
unsigned dg_film_pose(const dg_member_t *m, const dg_sprite_t *s) {
  if (!m || m->type != 2 || !m->film_count)
    return 0;
  return m->film_loop                    ? s->film_frame % m->film_count
         : s->film_frame < m->film_count ? s->film_frame
                                         : m->film_count - 1u;
}
static void set_member(dg_sprite_t *s, uint32_t id) {
  if (s->value.member != id)
    s->film_frame = 0;
  s->value.member = id;
}

static bool eq_full(const char *a, const char *b) {
  while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
    a++;
    b++;
  }
  return *a == *b;
}
// Property and command dispatch is a long chain of eq(name, "literal") against
// one name, and the target builds at -Os, so every link is a real call.
// Rejecting on the first folded character inline turns almost all of them into
// a single compare. ASCII folds exactly as tolower does; anything else still
// goes to the full walk.
static inline bool eq(const char *a, const char *b) {
  unsigned char x = (unsigned char)a[0], y = (unsigned char)b[0];
  if (x < 128 && y < 128 &&
      (x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y))
    return false;
  return eq_full(a, b);
}
#if DG_D10
const dg_flash_field_t *dg_flash_field_find(const dg_member_t *m,
                                            const char *name) {
  if (!m || !name || !*name)
    return NULL;
  for (unsigned i = 0; i < m->flash_field_count; i++)
    if (eq(m->flash_fields[i].name, name))
      return &m->flash_fields[i];
  return NULL;
}
// Decode one UTF-8 sequence. An ill-formed byte passes through as its own
// codepoint so converted windows-1252 text can never wedge the walk.
static unsigned dg_utf8_next(const char **text) {
  const unsigned char *s = (const unsigned char *)*text;
  unsigned cp = s[0], extra = 0;
  if (cp >= 0xf0) { cp &= 7; extra = 3; }
  else if (cp >= 0xe0) { cp &= 15; extra = 2; }
  else if (cp >= 0xc0) { cp &= 31; extra = 1; }
  unsigned used = 1;
  for (; used <= extra; used++) {
    if ((s[used] & 0xc0) != 0x80) { cp = s[0]; used = 1; break; }
    cp = cp << 6 | (s[used] & 63u);
  }
  *text += used;
  return cp;
}
// A measured advance where the table covers the codepoint; half an em per
// character is the recorded metrics fallback everywhere else.
static unsigned dg_style_advance(const dg_text_style_t *style, unsigned cp) {
  if (style && style->advances && cp >= 32 && cp - 32 < style->advance_count &&
      style->advances[cp - 32])
    return style->advances[cp - 32];
  return style ? ((unsigned)style->size + 1) / 2 : 6;
}
unsigned dg_flash_field_wrap(const dg_flash_field_t *f, unsigned size,
                             const char *text, int width, char *out,
                             unsigned capacity) {
  const dg_text_style_t *style = f->style;
  unsigned base = style && style->size ? style->size : 12;
  if (!size)
    size = base;
  // The SWF renderer keeps a two-pixel gutter inside the field bounds.
  int inner = width - f->margin_left - f->margin_right - 4;
  if (inner < 1)
    inner = 1;
  unsigned lines = 1, used = 0, line_start = 0, break_out = 0;
  int pen = f->indent, pen_break = 0;
  while (*text && used + 8 < capacity) {
    const char *at = text;
    unsigned cp = dg_utf8_next(&text);
    if (cp == '\r' || cp == '\n') {
      if (cp == '\r' && *text == '\n')
        text++;
      out[used++] = '\n';
      lines++;
      pen = 0;
      break_out = 0;
      line_start = used;
      continue;
    }
    int advance = (int)(dg_style_advance(style, cp) * size / base);
    if (f->word_wrap && cp != ' ' && pen + advance > inner &&
        break_out > line_start) {
      // The last space becomes the break; the carried word keeps its width.
      out[break_out] = '\n';
      lines++;
      pen -= pen_break;
      line_start = break_out + 1;
      break_out = 0;
      pen_break = 0;
    }
    if (cp == ' ') {
      break_out = used;
      pen_break = pen + advance;
    }
    while (at < text && used + 1 < capacity)
      out[used++] = *at++;
    pen += advance;
  }
  out[used] = 0;
  unsigned line_height = size + (unsigned)(f->leading < 0 ? 0 : f->leading);
  return lines * line_height;
}
#endif
static void error(dg_runtime_t *d, const char *operation, const char *name) {
  char text[100];
  snprintf(text, sizeof(text), "%.40s: %.56s", operation, name);
  lv_fail(d->values, text);
}
// Failures the original raises as a script alert before playing on.
static void script_error(dg_runtime_t *d, const char *operation,
                         const char *name) {
  char text[100];
  snprintf(text, sizeof(text), "%.40s: %.56s", operation, name);
  lv_script_fail(d->values, text);
}
static bool recover_script_error(dg_runtime_t *d) {
  lv_runtime_t *r = d->values;
  // Answer the question lv_recover_script would, before spending anything on
  // a report: the service loop asks on the way past whenever a failure is
  // pending, and most of those are not recoverable script errors.
  if (!r->failed || !r->script_error)
    return false;
  char text[192];
  snprintf(text, sizeof(text), "SCRIPT_ERROR %s", r->error);
  // Recovery discards every call context, so the chain that reached the
  // failing handler has to be read off before it goes. The innermost frame
  // is the one the message already names; what costs an afternoon is the
  // caller that passed it the wrong thing.
  char chain[192];
  unsigned at = (unsigned)snprintf(chain, sizeof(chain), "CALL_CHAIN");
  for (unsigned i = r->depth; i-- > 0 && at + 48 < sizeof(chain);)
    at += (unsigned)snprintf(chain + at, sizeof(chain) - at, " %.20s:%.20s:%u",
                             r->frames[i].movie ? r->frames[i].movie->name : "?",
                             r->frames[i].handler ? r->frames[i].handler->name : "?",
                             r->frames[i].line);
  if (!lv_recover_script(r))
    return false;
  if (d->platform.trace) {
    d->platform.trace(d->context, text);
    d->platform.trace(d->context, chain);
  }
  return true;
}
static void set_position(dg_runtime_t *d, dg_sprite_t *s, int x, int y) {
#if DG_CAP_CONSTRAINTS
  if (s->constraint > 0 && s->constraint < DG_SPRITES) {
    int l, t, r, b;
    dg_bounds(d, (unsigned)s->constraint, &l, &t, &r, &b);
    // Channel::setPosition constrains the registration point, inclusively.
    x = x < l ? l : x > r ? r : x;
    y = y < t ? t : y > b ? b : y;
  }
#else
  (void)d;
#endif
  s->value.x = x;
  s->value.y = y;
}
#if DG_CAP_KEYBOARD
void dg_key(dg_runtime_t *d, unsigned code, unsigned character, bool down) {
  if (code >= 128 || character > 255) {
    error(d, "keyboard input", "out of range");
    return;
  }
  uint32_t bit = UINT32_C(1) << (code & 31);
  bool held = (d->keys_down[code >> 5] & bit) != 0;
  if (held == down) return;
  if (d->key_count == 32) {
    error(d, "keyboard input", "event queue full");
    return;
  }
  unsigned index = (d->key_first + d->key_count++) % 32;
  d->key_events[index].code = code;
  d->key_events[index].character = down ? character : d->key_chars[code];
  d->key_events[index].down = down;
  if (down) {
    d->keys_down[code >> 5] |= bit;
    d->key_chars[code] = (uint8_t)character;
  } else {
    d->keys_down[code >> 5] &= ~bit;
    d->key_chars[code] = 0;
  }
}
#endif
const dg_movie_t *dg_loaded(dg_runtime_t *d, unsigned file) {
  for (unsigned i = 0; i < d->loaded_count; i++)
    if (d->loaded[i]->id == file)
      return d->loaded[i];
  return NULL;
}
const dg_member_t *dg_member(dg_runtime_t *d, uint32_t id) {
  if (!id)
    return NULL;
#if DG_EXTENDED
  for (unsigned i = 0; i < d->dynamic_count; i++)
    if (d->dynamic_members[i].member.id == id) return &d->dynamic_members[i].member;
#endif
  const dg_movie_t *movie = dg_loaded(d, id >> 20);
  if (movie) {
    unsigned first = 0, end = movie->member_count;
    while (first < end) {
      unsigned middle = first + (end - first) / 2;
      uint32_t key = movie->members[middle].id;
      if (key == id)
        return &movie->members[middle];
      if (key < id)
        first = middle + 1;
      else
        end = middle;
    }
  }
  return NULL;
}
#if DG_MODERN
static bool member_looping(dg_runtime_t *d, const dg_member_t *m) {
  for (unsigned i = 0; i < d->loop_count; i++)
    if (d->loop_members[i] == m->id)
      return d->loop_values[i];
  return m->type == 2 ? m->film_loop : m->looping;
}
#endif
static void play_member(dg_runtime_t *d, unsigned channel,
                        const dg_member_t *m) {
  if (!d->platform.sound)
    return;
#if DG_MODERN
  if (m) {
    // Callbacks consume this snapshot synchronously, like the immutable member
    // metadata previously passed here. Persistent overrides retain IDs only.
    dg_member_t effective = *m;
    effective.looping = member_looping(d, m);
    if (effective.looping && effective.loop_end <= effective.loop_start) {
      effective.loop_start = 0;
      effective.loop_end = effective.samples;
    }
    d->platform.sound(d->context, channel, &effective);
    return;
  }
#endif
  d->platform.sound(d->context, channel, m);
}
static uint32_t reference_id(dg_runtime_t *d, unsigned cast, unsigned member) {
  if (!member)
    return 0;
  if (!cast || cast > d->movie->cast_count || member > 65535) {
    error(d, "invalid cast", "reference");
    return 0;
  }
  const dg_cast_t *lib = &d->movie->casts[cast - 1];
#if DG_D10
  const dg_movie_t *swap = d10_cast_target(d, lib->name);
  // A swapped library's members live in the target archive's External cast.
  if (swap)
    return swap->id << 20 | 1u << 16 | member;
#endif
  return lib->file << 20 | lib->cast << 16 | member;
}
static unsigned member_cast(dg_runtime_t *d, uint32_t id) {
  for (unsigned c = 0; c < d->movie->cast_count; c++) {
#if DG_D10
    const dg_movie_t *swap = d10_cast_target(d, d->movie->casts[c].name);
    if (swap) {
      if (swap->id == id >> 20 && ((id >> 16) & 15) == 1)
        return c + 1;
      continue;
    }
#endif
    if (d->movie->casts[c].file == (id >> 20) &&
        d->movie->casts[c].cast == ((id >> 16) & 15))
      return c + 1;
  }
  return 0;
}
#if DG_MODERN
static bool format_reference(lv_runtime_t *r, lv_t value, char *out,
                              size_t capacity) {
  dg_runtime_t *d = r->context;
  int n;
  if (lv_type(value) == LV_CASTLIB)
    n = snprintf(out, capacity, "castLib %ld", (long)lv_id(value));
  else if (lv_type(value) == LV_MEMBER || lv_type(value) == LV_FIELD)
    n = snprintf(out, capacity, "member %u of castLib %u",
                 (unsigned)lv_id(value) & 65535, member_cast(d, (uint32_t)lv_id(value)));
  else
    return false;
  if (n < 0 || (size_t)n >= capacity)
    lv_fail(r, "reference string capacity");
  return true;
}
static const lv_handler_t *resolve_script(lv_runtime_t *r, lv_t script,
                                          const char *name,
                                          const lv_movie_t **owner) {
  dg_runtime_t *d = r->context;
  const dg_movie_t *movie = dg_loaded(d, (uint32_t)lv_id(script) >> 20);
  unsigned cast = ((uint32_t)lv_id(script) >> 16) & 15;
  if (!movie || !cast || cast > movie->cast_count)
    return NULL;
  const char *cast_name = movie->casts[cast - 1].name;
  unsigned member = (uint32_t)lv_id(script) & 65535;
  if (name) {
    // The generated handler index answers a named lookup from one bucket.
    const lv_handler_t *h = lv_scan(movie->code, name, member, cast_name);
    if (h) *owner = movie->code;
    return h;
  }
  for (unsigned i = 0; i < movie->code->count; i++) {
    const lv_handler_t *h = &movie->code->handlers[i];
    if (h->member == member && eq(h->cast, cast_name)) {
      *owner = movie->code;
      return h;
    }
  }
  return NULL;
}
static lv_t script_identity(lv_runtime_t *r, const lv_movie_t *code,
                            const lv_handler_t *handler) {
  dg_runtime_t *d = r->context;
  for (unsigned i = 0; i < d->loaded_count; i++) {
    const dg_movie_t *movie = d->loaded[i];
    if (movie->code != code)
      continue;
    for (unsigned cast = 0; cast < movie->cast_count; cast++)
      if (eq(movie->casts[cast].name, handler->cast))
        return lv_make(LV_SCRIPT, (int32_t)(movie->id << 20 | (cast + 1) << 16 |
                                      handler->member));
  }
  return (lv_t){0};
}
#endif
uint32_t dg_member_name_hash(const char *name) { return lv_text_hash(name); }
// The member number a name search would have found by walking the whole table.
// Every candidate still goes through eq, so a hash collision costs one extra
// comparison and folding disagreements cannot resolve the wrong member.
static unsigned indexed_member(const dg_movie_t *file, unsigned cast,
                               const char *name, uint32_t hash) {
  // Hand-written movies (tests, fixtures) carry no index; walk the table the
  // way this always did rather than making the index a construction rule.
  if (!file->member_index) {
    for (unsigned i = 0; i < file->member_count; i++)
      if (file->members[i].cast == cast && eq(file->members[i].name, name))
        return file->members[i].number;
    return 0;
  }
  unsigned low = 0, high = file->member_count;
  while (low < high) {
    unsigned middle = low + (high - low) / 2;
    if (file->member_index[middle].hash < hash)
      low = middle + 1;
    else
      high = middle;
  }
  for (unsigned i = low;
       i < file->member_count && file->member_index[i].hash == hash; i++) {
    const dg_member_t *m = &file->members[file->member_index[i].member];
    if (m->cast == cast && eq(m->name, name))
      return m->number;
  }
  return 0;
}
static lv_t reference(lv_runtime_t *r, const char *kind, lv_t value,
                      lv_t library) {
  unsigned ln = lv_name_id(r, kind);
  dg_runtime_t *d = r->context;
#if DG_MODERN
  if (ln == LN_CASTLIB) {
    int32_t cast = 0;
    if (lv_type(value) == LV_STRING || lv_type(value) == LV_SYMBOL) {
#if DG_D10
      // Resident controller-window casts answer under their encoded ids.
      cast = d10_castlib_id(d, lv_cstr(r, value));
#else
      const char *wanted = lv_cstr(r, value);
      for (unsigned c = 0; c < d->movie->cast_count; c++)
        if (eq(wanted, d->movie->casts[c].name)) {
          cast = (int32_t)c + 1;
          break;
        }
#endif
    } else
      cast = (int32_t)lv_integer(r, value);
    return lv_make(LV_CASTLIB, cast);
  }
#endif
  if (lv_type(value) == LV_MEMBER || lv_type(value) == LV_FIELD
#if DG_MODERN
      || lv_type(value) == LV_SCRIPT
#endif
  ) {
#if DG_MODERN
    if (ln == LN_SCRIPT)
      value = lv_make(LV_SCRIPT, lv_id(value));
#endif
    return value;
  }
  unsigned cast = 1, member = 0;
  if (lv_type(library) == LV_STRING || lv_type(library) == LV_SYMBOL) {
    cast = 0;
    const char *wanted = lv_cstr(r, library);
    for (unsigned i = 0; i < d->movie->cast_count; i++)
      if (eq(d->movie->casts[i].name, wanted))
        cast = i + 1;
    if (!cast) {
#if DG_D10
      // Resident controller-window code names its own cast libraries while
      // any stage movie plays; resolve them against the window's castlist.
      uint32_t window_id = 0;
      int window_hit = d10_window_member(d, lv_cstr(r, library), value, &window_id);
      if (window_hit == 1)
        return lv_make((ln == LN_FIELD)    ? LV_FIELD
                              : (ln == LN_SCRIPT) ? LV_SCRIPT
                                                   : LV_MEMBER, (int32_t)window_id);
      if (window_hit == 2) {
        // The window cast exists but has no member of that name: authored
        // probes observe #empty, exactly as an absent stage member does.
        if (!(ln == LN_SCRIPT))
          return lv_make((ln == LN_FIELD) ? LV_FIELD : LV_MEMBER, 0);
        error(d, "unknown member name", lv_cstr(r, value));
        return (lv_t){0};
      }
#endif
      error(d, "unknown cast library", lv_cstr(r, library));
      return (lv_t){0};
    }
  } else if (lv_type(library) == LV_CASTLIB) {
#if DG_D10
    if (lv_id(library) >= 256) {
      const dg_cast_t *window_lib = d10_castlib(d, lv_id(library));
      uint32_t window_id = 0;
      int hit = window_lib
                    ? d10_window_member(d, window_lib->name, value, &window_id)
                    : 0;
      if (hit == 1)
        return lv_make((ln == LN_FIELD)    ? LV_FIELD
                              : (ln == LN_SCRIPT) ? LV_SCRIPT
                                                   : LV_MEMBER, (int32_t)window_id);
      if (hit == 2 && !(ln == LN_SCRIPT))
        return lv_make((ln == LN_FIELD) ? LV_FIELD : LV_MEMBER, 0);
      error(d, "unknown cast library", "window");
      return (lv_t){0};
    }
#endif
    cast = (unsigned)lv_id(library);
  } else if (lv_type(library) != LV_VOID)
    cast = (unsigned)lv_integer(r, library);
  if (lv_type(value) == LV_STRING || lv_type(value) == LV_SYMBOL) {
    // Every search below compares the same requested name against a whole
    // cast; resolving it once per scan instead of once per member is what
    // makes entering a behavior-heavy scene affordable.
    const char *wanted_member = lv_cstr(r, value);
    const uint32_t wanted_hash = dg_member_name_hash(wanted_member);
#if DG_EXTENDED
    for (unsigned i = 0; i < d->dynamic_count; i++) {
      const dg_member_t *m = &d->dynamic_members[i].member;
      if (!m->type || !eq(m->name, wanted_member)) continue;
      for (unsigned k = 0; k < d->movie->cast_count; k++)
        if ((lv_type(library) == LV_VOID || k + 1 == cast) &&
            d->movie->casts[k].file == m->id >> 20)
          return lv_make(LV_MEMBER, (int32_t)m->id);
    }
#endif
    if (!cast || cast > d->movie->cast_count) {
#if DG_D10
      // member(name, 0) — the castLibNum of an absent member — probes
      // nothing and answers the authored #empty reference.
      if (!(ln == LN_SCRIPT))
        return lv_make((ln == LN_FIELD) ? LV_FIELD : LV_MEMBER, 0);
#endif
      error(d, "invalid cast", "name search");
      return (lv_t){0};
    }
    const dg_cast_t *lib = &d->movie->casts[cast - 1];
    const dg_movie_t *file = dg_loaded(d, lib->file);
    unsigned lib_cast = lib->cast;
#if DG_D10
    {
      const dg_movie_t *swap = d10_cast_target(d, lib->name);
      if (swap) {
        file = swap;
        lib_cast = 1;
      }
    }
#endif
    if (file)
      member = indexed_member(file, lib_cast, wanted_member, wanted_hash);
#if DG_MODERN
    if (!member && lv_type(library) == LV_VOID) {
      for (unsigned k = 1; k <= d->movie->cast_count && !member; k++) {
        const dg_cast_t *other = &d->movie->casts[k - 1];
        const dg_movie_t *file = dg_loaded(d, other->file);
        unsigned other_cast = other->cast;
#if DG_D10
        {
          const dg_movie_t *swap = d10_cast_target(d, other->name);
          if (swap) {
            file = swap;
            other_cast = 1;
          }
        }
#endif
        if (!file)
          continue;
        member = indexed_member(file, other_cast, wanted_member, wanted_hash);
        if (member)
          cast = k;
      }
    }
#endif
    if (!member) {
#if DG_D10
      uint32_t window_id = 0;
      if (lv_type(library) == LV_VOID &&
          d10_window_member(d, NULL, value, &window_id) == 1)
        return lv_make((ln == LN_FIELD)    ? LV_FIELD
                              : (ln == LN_SCRIPT) ? LV_SCRIPT
                                                   : LV_MEMBER, (int32_t)window_id);
#endif
#if DG_MODERN
      if (!(ln == LN_SCRIPT))
        return lv_make((ln == LN_FIELD) ? LV_FIELD : LV_MEMBER, 0);
#endif
      error(d, "unknown member name", lv_cstr(r, value));
      return (lv_t){0};
    }
  } else {
    member = (unsigned)lv_integer(r, value);
#if DG_D5 || DG_D7_UP
    if(lv_type(library)==LV_VOID && member>=131072){cast=1+(member>>17);member&=131071;}
#endif
  }
  unsigned referenced_type = (ln == LN_FIELD) ? LV_FIELD :
#if DG_MODERN
                             (ln == LN_SCRIPT) ? LV_SCRIPT :
#endif
                             LV_MEMBER;
  return lv_make(referenced_type, (int32_t)reference_id(d, cast, member));
}
void dg_bounds(dg_runtime_t *d, unsigned index, int *left, int *top, int *right,
               int *bottom) {
  if (!index || index >= DG_SPRITES) {
    *left = *top = *right = *bottom = 0;
    return;
  }
  dg_sprite_bounds(d, &d->sprites[index], left, top, right, bottom);
}
static void sprite_rect(dg_runtime_t *d, const dg_sprite_t *s, int *left,
                        int *top, int *right, int *bottom) {
  const dg_member_t *m = dg_member(d, s->value.member);
  int width = s->value.width, height = s->value.height, rx = width / 2,
      ry = height / 2;
  if (m) {
    rx = m->reg_x;
    ry = m->reg_y;
    if (!s->stretch && ((m->width && m->height)
#if DG_EXTENDED
                        || m->type == 1
#endif
    )) {
      width = m->width;
      height = m->height;
    } else {
      if (m->width)
        rx = rx * width / m->width;
      if (m->height)
        ry = ry * height / m->height;
    }
  }
  *left = s->value.x - rx;
  *top = s->value.y - ry;
  *right = *left + width;
  *bottom = *top + height;
}
void dg_sprite_quad(dg_runtime_t *d, const dg_sprite_t *s, float q[8]) {
#if DG_MODERN
  if (s->custom_quad) {
    for (unsigned i = 0; i < 4; i++) {
      q[i * 2] = s->quad[i * 2] + s->value.x;
      q[i * 2 + 1] = s->quad[i * 2 + 1] + s->value.y;
    }
    return;
  }
#endif
  int l, t, r, b;
  sprite_rect(d, s, &l, &t, &r, &b);
  float x[4] = {l, r, r, l}, y[4] = {t, t, b, b};
#if DG_MODERN
  float radians = s->value.rotation * (3.14159265358979323846f / 18000.0f);
  float cs = cosf(radians), sn = sinf(radians);
  float shear = tanf(s->value.skew * (3.14159265358979323846f / 18000.0f));
  for (unsigned i = 0; i < 4; i++) {
    float xx = x[i] - s->value.x, yy = y[i] - s->value.y;
    xx += shear * yy;
    q[2 * i] = s->value.x + cs * xx - sn * yy;
    q[2 * i + 1] = s->value.y + sn * xx + cs * yy;
  }
#else
  for (unsigned i = 0; i < 4; i++) {
    q[2 * i] = x[i];
    q[2 * i + 1] = y[i];
  }
#endif
}
bool dg_quad_uv(const float q[8], float x, float y, float *u, float *v) {
  float bx = q[2] - q[0], by = q[3] - q[1], cx = q[6] - q[0], cy = q[7] - q[1];
  float ex = q[0] - q[2] + q[4] - q[6], ey = q[1] - q[3] + q[5] - q[7];
  float determinant = bx * cy - by * cx;
  if (fabsf(determinant) < 0.0001f)
    return false;
  *u = ((x - q[0]) * cy - (y - q[1]) * cx) / determinant;
  *v = (bx * (y - q[1]) - by * (x - q[0])) / determinant;
  if (ex || ey)
    for (unsigned i = 0; i < 6; i++) {
      float px = q[0] + bx * *u + cx * *v + ex * *u * *v - x;
      float py = q[1] + by * *u + cy * *v + ey * *u * *v - y;
      float ux = bx + ex * *v, uy = by + ey * *v, vx = cx + ex * *u,
            vy = cy + ey * *u;
      determinant = ux * vy - uy * vx;
      if (fabsf(determinant) < 0.0001f)
        return false;
      *u -= (px * vy - py * vx) / determinant;
      *v -= (ux * py - uy * px) / determinant;
    }
  return *u >= 0 && *u < 1 && *v >= 0 && *v < 1;
}
void dg_sprite_bounds(dg_runtime_t *d, const dg_sprite_t *s, int *left,
                      int *top, int *right, int *bottom) {
#if DG_MODERN
  if (s->value.rotation || s->value.skew || s->custom_quad) {
    float q[8];
    dg_sprite_quad(d, s, q);
    float l = q[0], t = q[1], r = q[0], b = q[1];
    for (unsigned i = 1; i < 4; i++) {
      if (q[2 * i] < l)
        l = q[2 * i];
      if (q[2 * i] > r)
        r = q[2 * i];
      if (q[2 * i + 1] < t)
        t = q[2 * i + 1];
      if (q[2 * i + 1] > b)
        b = q[2 * i + 1];
    }
    *left = floorf(l + 0.0001f);
    *top = floorf(t + 0.0001f);
    *right = ceilf(r - 0.0001f);
    *bottom = ceilf(b - 0.0001f);
    return;
  }
#endif
  sprite_rect(d, s, left, top, right, bottom);
}
unsigned dg_draw_order(dg_runtime_t *d, const uint16_t **out) {
  *out = d->order_cache;
  if (d->order_valid && d->order_cached == d->order_serial)
    return d->order_count;
  uint16_t *order = d->order_cache;
  unsigned count = 0;
  d->order_valid = true;
  d->order_cached = d->order_serial;
#if DG_MODERN
  // Walking every channel strides the whole sprite table, and this runs about
  // once per tick for the mouse hit test as well as once per render.
  // `active_sprites` is a superset of the occupied channels, so skipping its
  // empty words cannot drop a live sprite. Trails draw from every channel.
  if (!d->trail_count) {
    for (unsigned word = 0; word < (DG_SPRITES + 31) / 32; word++) {
      uint32_t bits = d->active_sprites[word];
      if (!bits) continue;
      unsigned base = word * 32, last = base + 32;
      if (last > DG_SPRITES) last = DG_SPRITES;
      for (unsigned i = base ? base : 1; i < last; i++) {
        if (!(bits & (UINT32_C(1) << (i & 31))) || !d->sprites[i].value.type)
          continue;
        unsigned at = count;
        while (at && d->sprites[order[at - 1]].value.loc_z >
                         d->sprites[i].value.loc_z) {
          order[at] = order[at - 1];
          at--;
        }
        order[at] = i;
        count++;
      }
    }
    d->order_count = count;
    return count;
  }
#endif
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    unsigned at = count;
#if DG_MODERN
    if (!d->sprites[i].value.type && !d->trail_count)
      continue;
    while (at &&
           d->sprites[order[at - 1]].value.loc_z > d->sprites[i].value.loc_z) {
      order[at] = order[at - 1];
      at--;
    }
#endif
    order[at] = i;
    count++;
  }
  d->order_count = count;
  return count;
}
static bool picture_changed(dg_runtime_t *d, const dg_sprite_t *a,
                            const dg_sprite_t *b) {
#if DG_MODERN
  if (a->custom_quad != b->custom_quad ||
      (a->custom_quad && memcmp(a->quad, b->quad, sizeof(a->quad))))
    return true;
  if (a->value.rotation != b->value.rotation ||
      a->value.skew != b->value.skew || a->value.loc_z != b->value.loc_z ||
      a->flip_h != b->flip_h || a->flip_v != b->flip_v ||
      a->value.fore_rgb != b->value.fore_rgb ||
      a->value.back_rgb != b->value.back_rgb)
    return true;
#endif
  if (a->visible != b->visible || a->stretch != b->stretch ||
      a->value.member != b->value.member || a->value.x != b->value.x ||
      a->value.y != b->value.y || a->value.width != b->value.width ||
      a->value.height != b->value.height || dg_opacity(a) != dg_opacity(b) ||
      a->value.type != b->value.type || a->value.ink != b->value.ink ||
      a->value.fore != b->value.fore || a->value.back != b->value.back ||
      (a->value.thickness & 15) != (b->value.thickness & 15))
    return true;
  if (a->film_frame == b->film_frame)
    return false;
  const dg_member_t *m = dg_member(d, a->value.member);
  return dg_film_pose(m, a) != dg_film_pose(m, b);
}
static bool overlaps(dg_runtime_t *d, const dg_sprite_t *a,
                     const dg_sprite_t *b) {
  int l, t, r, bb, ll, tt, rr, bbb;
  dg_sprite_bounds(d, a, &l, &t, &r, &bb);
  dg_sprite_bounds(d, b, &ll, &tt, &rr, &bbb);
  return l < rr && ll < r && t < bbb && tt < bb;
}
#if DG_MODERN
// Whether channel i's member is a film loop, refreshed when its member
// changes. Every member change reaches event_channel_changed, so this runs
// there; the frame advance then reads only the film channels.
static void film_channel_changed(dg_runtime_t *d, unsigned i) {
  const dg_sprite_t *s = &d->sprites[i];
  if (s->value.member == d->film_member[i]) return;
  d->film_member[i] = s->value.member;
  const dg_member_t *m = dg_member(d, s->value.member);
  bool loop = m && m->type == 2 && m->film_count;
  d->film_loop_channel[i] = loop;
  d->film_loop_member[i] = loop ? m : NULL;
  uint32_t bit = UINT32_C(1) << (i & 31);
  if (loop)
    d->film_channels[i >> 5] |= bit;
  else
    d->film_channels[i >> 5] &= ~bit;
}
static void event_channel_changed(dg_runtime_t *d, unsigned i) {
#if DG_D7_OR_D10
  d->mouse_hit_valid = false;
#endif
  film_channel_changed(d, i);
  uint32_t bit = UINT32_C(1) << (i & 31);
  for (unsigned phase = 0; phase < 3; phase++)
    d->event_quiet[phase][i >> 5] &= ~bit;
  bool eligible = d->sprites[i].value.member ||
                  (d->values &&
                   lv_type(d->values->roots[DG_BEHAVIOR_ROOT + i]) != LV_VOID);
  if (eligible)
    d->event_channels[i >> 5] |= bit;
  else
    d->event_channels[i >> 5] &= ~bit;
}
#endif
void dg_sprite_changed(dg_runtime_t *d, unsigned i) {
  if (!i || i >= DG_SPRITES)
    return;
  d->order_serial++;
  d->active_sprites[i >> 5] |= UINT32_C(1) << (i & 31);
#if DG_MODERN
  event_channel_changed(d, i);
#endif
  if (!d->stage_dirty[i]) {
    d->stage_dirty[i] = true;
    d->stage_channels[d->stage_count++] = (uint16_t)i;
  }
}
void dg_update_stage(dg_runtime_t *d) {
  // Trails preserve each committed image of a moved/recast sprite, even when
  // several updateStage calls occur between physical display refreshes.
  // A normal sprite's dirty area erases intersecting trails, including the
  // source game's hide/show of the arithmetic text panels to clear glyphs.
  for (unsigned n = 0; n < d->stage_count; n++) {
    unsigned i = d->stage_channels[n];
    const dg_sprite_t *s = &d->sprites[i], *old = &d->staged[i];
    if (!picture_changed(d, s, old))
      continue;
    d->visual_revision++;
    if (old->trails && old->visible && old->value.type && dg_opacity(old)) {
      if (d->trail_count == DG_TRAILS) {
        error(d, "sprite trail capacity", "exceeded");
        break;
      }
      d->trails[d->trail_count++] = (dg_trail_t){i, *old};
      d->order_serial++; // trails draw from every channel
    }
  }
  for (unsigned n = 0; n < d->stage_count; n++) {
    unsigned i = d->stage_channels[n];
    const dg_sprite_t *s = &d->sprites[i], *old = &d->staged[i];
    if (s->trails || old->trails || !picture_changed(d, s, old))
      continue;
    for (unsigned j = 0; j < d->trail_count;) {
      if ((s->value.type && overlaps(d, &d->trails[j].sprite, s)) ||
          (old->value.type && overlaps(d, &d->trails[j].sprite, old))) {
        memmove(&d->trails[j], &d->trails[j + 1],
                (--d->trail_count - j) * sizeof(d->trails[0]));
      } else
        j++;
    }
  }
  for (unsigned n = 0; n < d->stage_count; n++) {
    unsigned i = d->stage_channels[n];
    d->staged[i] = d->sprites[i];
    if (!d->sprites[i].value.type) {
      d->active_sprites[i >> 5] &= ~(UINT32_C(1) << (i & 31));
      d->order_serial++;
    }
    d->stage_dirty[i] = false;
  }
  d->stage_count = 0;
}
// Two questions about one member/script ID, both answered by walking the whole
// handler table of the owning movie: does its cast script take mouseDown or
// mouseUp, and does anything it declares take a mouse event at all.
enum { MOUSE_CAST = 1u, MOUSE_SCRIPT = 2u };
static bool declares_mouse(dg_runtime_t *d, uint32_t id, unsigned query) {
  const dg_movie_t *m = dg_loaded(d, id >> 20);
  unsigned cast = (id >> 16) & 15;
  if (!m || !cast || cast > m->cast_count)
    return false;
  for (unsigned i = 0; i < m->code->count; i++) {
    const lv_handler_t *h = &m->code->handlers[i];
    if (h->member != (id & 65535) || !eq(h->cast, m->casts[cast - 1].name))
      continue;
    if (query == MOUSE_CAST) {
      if (eq(h->kind, "CastScript") &&
          (eq(h->name, "mousedown") || eq(h->name, "mouseup")))
        return true;
    } else if (eq(h->name, "mousedown") || eq(h->name, "mouseup") ||
               eq(h->name, "mouseenter") || eq(h->name, "mouseleave") ||
               eq(h->name, "mousewithin"))
      return true;
  }
  return false;
}
static bool memo_mouse(dg_runtime_t *d, uint32_t id, unsigned query) {
  // Fowler-Noll-Vo style spread over the packed file/cast/member ID, whose low
  // bits alone would collide across every cast of one movie.
  unsigned slot = (id * UINT32_C(0x9e3779b1)) >> 26;
  if (d->mouse_memo[slot].id != id) {
    d->mouse_memo[slot].id = id;
    d->mouse_memo[slot].known = d->mouse_memo[slot].mouse = 0;
  }
  if (!(d->mouse_memo[slot].known & query)) {
    d->mouse_memo[slot].known |= (uint8_t)query;
    if (declares_mouse(d, id, query))
      d->mouse_memo[slot].mouse |= (uint8_t)query;
  }
  return d->mouse_memo[slot].mouse & query;
}
static bool cast_mouse(dg_runtime_t *d, uint32_t id) {
  return memo_mouse(d, id, MOUSE_CAST);
}
#if DG_EXTENDED
static bool mouse_behavior(dg_runtime_t *d, unsigned channel) {
  lv_runtime_t *r = d->values;
  lv_t list = r->roots[DG_BEHAVIOR_ROOT + channel];
  if (lv_type(list) != LV_LIST) return false;
  for (unsigned i = 1; i <= lv_count(r, list); i++) {
    lv_t receiver = lv_at(r, list, i);
    // The list and the ancestor chain are live state, so they are still walked
    // every time; only the per-script answer comes from the memo.
    for (unsigned depth = 0; lv_type(receiver) == LV_INSTANCE && depth < 64; depth++) {
      lv_t script = lv_get(r, NULL, "script", receiver);
      if (memo_mouse(d, (uint32_t)lv_id(script), MOUSE_SCRIPT))
        return true;
      receiver = lv_get(r, NULL, "ancestor", receiver);
    }
  }
  return false;
}
#endif
static unsigned hit_sprite(dg_runtime_t *d, int x, int y, bool active,
                            bool cursor_only) {
  const uint16_t *order;
  unsigned count = dg_draw_order(d, &order);
  while (count) {
    unsigned i = order[--count];
    dg_sprite_t *s = &d->sprites[i];
    if (!s->visible || !s->value.type)
      continue;
    if (cursor_only && !dg_cursor_active(s->cursor))
      continue;
#if !DG_EXTENDED
    if (active && !s->value.script && !s->moveable &&
        !cast_mouse(d, s->value.member))
      continue;
#endif
    int l, t, r, b;
    dg_bounds(d, i, &l, &t, &r, &b);
    if (x >= l && x < r && y >= t && y < b) {
#if DG_EXTENDED
      // A behavior that only animates a sprite is not a mouse receiver.
      // In particular, the source's moving cursor must not steal the button's
      // mouseLeave/mouseUp as its image changes in response to mouseEnter.
      if (active && !s->moveable && !dg_text_editable(d, i) && !mouse_behavior(d, i) &&
          !cast_mouse(d, s->value.member)) continue;
#endif
      const dg_member_t *m = dg_member(d, s->value.member);
      int tx = x - l, ty = y - t, tw = r - l, th = b - t;
#if DG_MODERN
      float q[8];
      dg_sprite_quad(d, s, q);
      float u, v;
      if (!dg_quad_uv(q, x, y, &u, &v))
        continue;
      if (s->flip_h)
        u = 1 - u;
      if (s->flip_v)
        v = 1 - v;
      tx = u * 65535;
      ty = v * 65535;
      tw = th = 65536;
#endif
      if (m && m->type == 1 && d->platform.hit &&
          !d->platform.hit(d->context, m, s->value.ink, tx * m->width / tw,
                           ty * m->height / th))
        continue;
      return i;
    }
  }
  return 0;
}
unsigned dg_hit(dg_runtime_t *d, int x, int y) {
  return hit_sprite(d, x, y, false, false);
}
unsigned dg_mouse_hit(dg_runtime_t *d, int x, int y) {
#if DG_D7_OR_D10
  // Frame dispatch repeatedly polls hover between script calls. Geometry and
  // behavior state are unchanged there; avoid rescanning the full sprite score.
  if (d->mouse_hit_valid && d->mouse_hit_x == x && d->mouse_hit_y == y)
    return d->mouse_hit_result;
  d->mouse_hit_x = x; d->mouse_hit_y = y;
  d->mouse_hit_result = hit_sprite(d, x, y, true, false);
  d->mouse_hit_valid = true;
  return d->mouse_hit_result;
#else
#if DG_MODERN && !DG_EXTENDED
  return hit_sprite(d, x, y, false, false);
#else
  return hit_sprite(d, x, y, true, false);
#endif
#endif
}
dg_cursor_t dg_cursor_at(dg_runtime_t *d, int x, int y) {
  // Score::renderCursor searches for a cursor-bearing channel independently
  // of click handlers, then uses the movie default (including hidden 200).
  unsigned channel = hit_sprite(d, x, y, false, true);
  return channel ? d->sprites[channel].cursor : d->cursor;
}
dg_cursor_t dg_cursor_current(dg_runtime_t *d) {
#if DG_D5
  if (d->native_dialog && d->suspended_stage &&
      (d->mouse_x < 0 || d->mouse_y < 0 || d->mouse_x >= 416 || d->mouse_y >= 240))
    return dg_cursor_at(d->suspended_stage, d->mouse_x + 112, d->mouse_y + 100);
#endif
  return dg_cursor_at(d, d->mouse_x, d->mouse_y);
}

static void cursor_set(dg_runtime_t *d, dg_cursor_t *target, lv_t value) {
  lv_runtime_t *r = d->values;
  dg_cursor_t next = {0};
  if (lv_type(value) == LV_LIST) {
    unsigned count = lv_count(r, value);
    if (!count) { error(d, "cursor bitmap list", "empty"); return; }
    for (unsigned i = 0; i < 2 && i < count; i++) {
      lv_t item = lv_at(r, value, i + 1), ref;
      if (lv_type(item) == LV_NUMBER) {
        int32_t n = lv_integer(r, item);
        if (i && !n) continue; // Getter represents an absent mask as zero.
        if (n < 0) { error(d, "cursor bitmap member", "negative"); return; }
        ref = reference(r, "member", lv_num(n & 131071), lv_num(1 + (n >> 17)));
      } else ref = reference(r, "member", item, (lv_t){0});
      const dg_member_t *m = dg_member(d, (uint32_t)lv_id(ref));
      if (r->failed) return;
#if DG_D7_OR_D10
      // D7 readFromCast leaves the current cursor intact for an absent cast.
      // DAG13 retains references 2000/2001 without corresponding media.
      if (!m) {
        if (d->platform.trace) d->platform.trace(d->context, "CURSOR_MEMBER_EMPTY");
        return;
      }
#endif
      if (!m || m->type != 1 || !m->width || !m->height) {
        error(d, "cursor bitmap member", i ? "invalid mask" : "invalid image");
        return;
      }
      if (i) next.mask = m->id; else next.image = m->id;
    }
  } else {
    if (lv_type(value) != LV_NUMBER) {
      error(d, "cursor value", "expected built-in number or bitmap list");
      return;
    }
    next.resource = lv_integer(r, value);
    if (r->failed) return;
    if ((next.resource < -1 || next.resource > 4) && next.resource != 200) {
#if DG_D10
      // The corpus selects Cursor Xtra resources — a pinned deferred
      // conversion. The pointer keeps its current shape, visibly traced.
      if (d->platform.trace) {
        char text[64];
        snprintf(text, sizeof(text), "CURSOR_RESOURCE_PENDING %ld",
                 (long)next.resource);
        d->platform.trace(d->context, text);
      }
      return;
#else
      error(d, "unsupported cursor resource", "only built-ins and bitmap lists recovered");
      return;
#endif
    }
  }
  *target = next;
}
static lv_t cursor_get(dg_runtime_t *d, dg_cursor_t value) {
  if (!value.image) return lv_num(value.resource);
  uint32_t ids[] = {value.image, value.mask};
  lv_t pair[2];
  for (unsigned i = 0; i < 2; i++) {
    unsigned cast = member_cast(d, ids[i]);
    pair[i] = lv_num(ids[i] ? (ids[i] & 65535) + (cast ? (cast - 1) << 17 : 0) : 0);
  }
  return lv_list(d->values, 2, pair, false);
}
static dg_sprite_t *sprite(dg_runtime_t *d, lv_t owner) {
  if (lv_type(owner) != LV_SPRITE || lv_id(owner) < 0 || lv_id(owner) >= DG_SPRITES) {
#if DG_D10
    // Rollover helpers probe offset channels beyond the score; an absent
    // sprite answers empty values, exactly as the original.
    if (lv_type(owner) == LV_SPRITE) return NULL;
#endif
    error(d, "invalid sprite", "property access");
    return NULL;
  }
  return &d->sprites[lv_id(owner)];
}
static int field(dg_runtime_t *d, uint32_t member) {
  for (unsigned i = 0; i < d->field_count; i++)
    if (d->field_members[i] == member)
      return (int)i;
  if (d->field_count == DG_FIELDS) {
    error(d, "field capacity", "exceeded");
    return -1;
  }
  unsigned i = d->field_count++;
  d->field_members[i] = member;
  const dg_member_t *m = dg_member(d, member);
  d->values->roots[DG_FIELD_COLOR_ROOT + i] = (lv_t){0};
  d->values->roots[DG_FIELD_TEXT_ROOT + i] =
      lv_text(d->values, m ? m->text : "", false);
#if DG_MODERN
  snprintf(d->field_fonts[i], sizeof(d->field_fonts[i]), "%s",
           m && m->text_style && m->text_style->font_name
               ? m->text_style->font_name : "");
  d->field_text_changed[i] = false;
  d->field_scroll[i] = 0;
#if DG_EXTENDED
  d->field_editable[i] = m && m->editable;
#endif
#endif
  return (int)i;
}
#if DG_EXTENDED
#include "d6.inc"
#endif
#if DG_D5
static unsigned frame_number(dg_runtime_t *d, lv_t value);
#include "d5.inc"
#endif
#if DG_D10
#include "d10.inc"
#endif
#if DG_EXTENDED
static lv_t behavior_property(dg_runtime_t *d, unsigned channel, const char *name) {
  lv_runtime_t *r = d->values;
  lv_t list = r->roots[DG_BEHAVIOR_ROOT + channel];
  if (lv_type(list) == LV_LIST) for (unsigned i = 1; i <= lv_count(r, list); i++) {
    lv_t instance = lv_at(r, list, i);
    if (lv_type(instance) != LV_INSTANCE) continue;
    if (lv_has_property(r, instance, name)) return instance;
    // Declared behavior properties exist before their first assignment. The
    // instance getter already returns VOID for those uninitialized values.
    const lv_movie_t *owner = NULL;
    lv_t script = lv_get(r, NULL, "script", instance);
    const lv_handler_t *handler = resolve_script(r, script, NULL, &owner);
    if (handler) for (unsigned p = 0; p < handler->property_count; p++)
      if (eq(handler->property_names[p], name)) return instance;
  }
  return (lv_t){0};
}
#endif
#if DG_MODERN
// In a frame of its own: the getter runs for every property read, and a
// 2 KB zeroed label array in its frame cost a memset per read and pushed
// every callee that far down the C stack.
static __attribute__((noinline)) lv_t marker_list(lv_runtime_t *r, dg_runtime_t *d) {
  lv_t labels[256] = {0};
  if (d->movie->label_count > 256) {
    error(d, "marker list capacity", "exceeded");
    return (lv_t){0};
  }
  lv_t result = lv_list(r, d->movie->label_count, labels, false);
  r->roots[DG_SCRATCH_ROOT] = result;
  for (unsigned i = 0; i < d->movie->label_count; i++)
    lv_set_at(r, result, i + 1, lv_text(r, d->movie->labels[i].name, false));
  return result;
}
#endif
static lv_t get(lv_runtime_t *r, const char *name, lv_t owner) {
  unsigned ln = lv_name_id(r, name);
  dg_runtime_t *d = r->context;
#if DG_EXTENDED
  lv_t d6_result = {0};
  if (d6_get(d, name, owner, &d6_result)) return d6_result;
#endif
#if DG_D5
  lv_t result={0};if(d5_get(d,name,owner,&result))return result;
#endif
#if DG_D10
  {lv_t d10_result={0};if(d10_get(d,name,owner,&d10_result))return d10_result;}
#endif
  if (lv_type(owner) == LV_VOID) {
    if ((ln == LN_LONGDATE) && d->platform.long_date)
      return lv_text(r, d->platform.long_date(d->context), false);
#if DG_MODERN
    if (ln == LN_EXITLOCK)
      return lv_num(d->exit_lock);
    if (ln == LN_SEARCHCURRENTFOLDER)
      return lv_num(d->search_current_folder);
    if (ln == LN_SEARCHPATHS)
      return r->roots[0];
    if (ln == LN_APPLICATIONPATH)
      return lv_text(r, "C:\\", false);
    if (ln == LN_PLATFORM)
      return lv_text(r, "Windows,32", false);
    if (ln == LN_STAGECOLOR)
      return lv_make(LV_COLOR, (int32_t)d->stage_color);
    if (ln == LN_STAGE) {
      lv_t rect = lv_rect(r, 0, 0, 640, 480);
      return lv_list(r, 4,
                     (lv_t[]){lv_text(r, "rect", true), rect,
                              lv_text(r, "sourcerect", true), rect}, true);
    }
    if ((ln == LN_MOUSEMEMBER) || (ln == LN_MOUSECAST)) {
      unsigned hit = dg_hit(d, d->mouse_x, d->mouse_y);
      if (ln == LN_MOUSECAST) {
        uint32_t id = hit ? d->sprites[hit].value.member : 0;
        unsigned cast = id ? member_cast(d, id) : 0;
        return lv_num(cast ? (int)((id & 65535) + ((cast - 1) << 17)) : 0);
      }
      return hit && d->sprites[hit].value.member
                 ? lv_make(LV_MEMBER, (int32_t)d->sprites[hit].value.member)
                 : (lv_t){0};
    }
    if ((ln == LN_KEYCODE) || (ln == LN_SHIFTDOWN)) {
#if DG_CAP_KEYBOARD
      return lv_num((ln == LN_KEYCODE) ? d->key_code :
                    !!(d->keys_down[56 >> 5] & (UINT32_C(1) << (56 & 31))));
#else
      return lv_num(0);
#endif
    }
    if (ln == LN_NUMBEROFXTRAS)
      return lv_num(2);
    if (ln == LN_MILLISECONDS)
      return lv_num(d->clock / 60000);
    if (ln == LN_MOUSELOC)
      return lv_point(r, d->mouse_x, d->mouse_y);
    if (ln == LN_CURRENTSPRITENUM)
      return lv_num(d->current_event_sprite);
    if (ln == LN_MOVIEPATH)
      return lv_text(r, "C:\\", false);
    if ((ln == LN_MOVIENAME) || (ln == LN_MOVIE))
      return lv_text(r, d->movie->code->name, false);
    if ((ln == LN_STAGELEFT) || (ln == LN_STAGETOP))
      return lv_num(0);
    if (ln == LN_STAGERIGHT)
      return lv_num(640);
    if (ln == LN_STAGEBOTTOM)
      return lv_num(480);
    if (ln == LN_DESKTOPRECTLIST)
      return lv_list(r, 1, (lv_t[]){lv_rect(r, 0, 0, 640, 480)}, false);
    if (ln == LN_MARKERLIST)
      return marker_list(r, d);
    if (ln == LN_FRAMELABEL) {
      // The label *of this frame*, empty where it has none -- not the last
      // label at or before it, which is what marker(0) answers. Willy's login
      // screen reads it in the beginSprite of a one-frame sprite dropped on
      // each of Still, Talk and Wait to name the section just entered.
      const char *label = "";
      for (unsigned i = 0; i < d->movie->label_count; i++)
        if (d->movie->labels[i].frame == d->frame)
          label = d->movie->labels[i].name;
      return lv_text(r, label, false);
    }
    if (ln == LN_LASTEVENT)
      return lv_num(0);
    if ((ln == LN_KEY) || (ln == LN_KEYPRESSED)) {
#if DG_CAP_KEYBOARD
      char text[2] = {(char)d->key_character, 0};
      // KB polls the property to distinguish a held direction from release.
      if ((ln == LN_KEYPRESSED) && !(d->keys_down[0] | d->keys_down[1] |
                                  d->keys_down[2] | d->keys_down[3])) text[0] = 0;
      return lv_text(r, text, false);
#else
      return lv_text(r, "", false);
#endif
    }
    if (ln == LN_DOUBLECLICK)
      return lv_num(0);
#endif
    if (ln == LN_MOUSEH)
      return lv_num(d->mouse_x);
    if (ln == LN_MOUSEV)
      return lv_num(d->mouse_y);
    if ((ln == LN_MOUSEDOWN) || (ln == LN_STILLDOWN))
      return lv_num(d->mouse_down);
    if (ln == LN_MOUSEUP)
      return lv_num(!d->mouse_down);
    if (ln == LN_CLICKON)
      return lv_num(d->click_on);
    if (ln == LN_TIMER)
      return lv_num(d->ticks - d->timer);
    if (ln == LN_TICKS)
      return lv_num(d->ticks);
    if (ln == LN_FRAME)
      return lv_num(d->frame);
    if (ln == LN_SEARCHPATH)
      return r->roots[0];
    if (ln == LN_PATHNAME)
      return lv_text(r, "C:\\", false);
    if (ln == LN_MACHINETYPE)
      return lv_num(256);
#if DG_EXTENDED
    if (ln == LN_MOUSEDOWNSCRIPT) return lv_text(r, d->mouse_down_script, false);
    if (ln == LN_KEYDOWNSCRIPT) return lv_text(r, d->key_down_script, false);
#endif
    if (ln == LN_MOUSEUPSCRIPT)
      return lv_text(r, d->mouse_up_script, false);
    if (ln == LN_SOUNDLEVEL)
      return lv_num(d->sound_level);
    if (ln == LN_COLORDEPTH)
      return lv_num(d->color_depth);
    if (ln == LN_FLOATPRECISION)
      return lv_num(d->float_precision);
    if (ln == LN_CURSOR)
      return cursor_get(d, d->cursor);
    if (ln == LN_MOVIERATE)
      return lv_num(1);
  } else if (lv_type(owner) == LV_SPRITE) {
    dg_sprite_t *s = sprite(d, owner);
    if (!s)
#if DG_D10
      // An absent sprite's member is the #empty member reference.
      return (ln == LN_MEMBER) ? lv_make(LV_MEMBER, 0) : (lv_t){0};
#else
      return (lv_t){0};
#endif
#if DG_D7_OR_D10
    if (ln == LN_CASTNUM)
      return lv_num(s->value.member ? ((member_cast(d, s->value.member)-1)<<17) | (s->value.member&65535) : 0);
    // A sprite answers the library of the member it shows; exercise
    // behaviors keep it to name sibling members in the same library.
    if (ln == LN_CASTLIBNUM)
      return lv_num(s->value.member ? member_cast(d, s->value.member) : 0);
#endif
    if ((ln == LN_CASTNUM) || (ln == LN_MEMBERNUM))
      return lv_num(s->value.member & 65535);
#if DG_MODERN
    if (ln == LN_TYPE)
      return lv_num(s->value.type);
    if (ln == LN_MEMBER)
      return lv_make(LV_MEMBER, (int32_t)s->value.member);
    if (ln == LN_SPRITENUM)
      return lv_num(lv_id(owner));
#if DG_CAP_CONSTRAINTS
    if (ln == LN_CONSTRAINT)
      return lv_num(s->constraint);
#endif
    if (ln == LN_LOCZ)
      return lv_num(s->value.loc_z);
    if (ln == LN_ROTATION)
      return lv_num(s->value.rotation / 100.0);
    if (ln == LN_SKEW)
      return lv_num(s->value.skew / 100.0);
    if (ln == LN_INK)
      return lv_num(s->value.ink);
    if (ln == LN_FLIPH)
      return lv_num(s->flip_h);
    if (ln == LN_FLIPV)
      return lv_num(s->flip_v);
    if (ln == LN_QUAD) {
      float q[8];
      dg_sprite_quad(d, s, q);
      lv_t result = lv_list(r, 4, (lv_t[4]){0}, false);
      r->roots[DG_SCRATCH_ROOT] = result;
      for (unsigned i = 0; i < 4; i++) {
        lv_t point = lv_point(r, q[i * 2], q[i * 2 + 1]);
        lv_set_at(r, result, i + 1, point);
      }
      return result;
    }
    if (ln == LN_SCRIPTINSTANCELIST)
      return r->roots[DG_BEHAVIOR_ROOT + lv_id(owner)];
#endif
    if (ln == LN_ROLLOVER) {
      // sprite(n).rollOver is rollOver(n) written the other way round, and
      // answers the same rectangle test. Christmas drags its baking
      // ingredients with it.
      int l, t, right, bottom;
      dg_bounds(d, (unsigned)lv_id(owner), &l, &t, &right, &bottom);
      return lv_num(d->mouse_x >= l && d->mouse_x < right &&
                    d->mouse_y >= t && d->mouse_y < bottom);
    }
    if (ln == LN_PUPPET)
      return lv_num(s->puppet);
    if (ln == LN_LOCH)
      return lv_num(s->value.x);
    if (ln == LN_LOCV)
      return lv_num(s->value.y);
    if (ln == LN_LOC)
      return lv_point(r, s->value.x, s->value.y);
    if (ln == LN_VISIBLE)
      return lv_num(s->visible);
    if (ln == LN_BLEND)
      return lv_num(s->value.blend);
    if (ln == LN_STRETCH)
      return lv_num(s->stretch);
    if (ln == LN_TRAILS)
      return lv_num(s->trails);
    if (ln == LN_MOVEABLESPRITE)
      return lv_num(s->moveable);
    if (ln == LN_CURSOR)
      return cursor_get(d, s->cursor);
    int l, t, rr, b;
    if ((ln == LN_WIDTH) || (ln == LN_HEIGHT)) {
      // Dimensions precede rotation/skew/quad deformation. TS.DXR's piston
      // feeds height back into its next quad; using the transformed bounds
      // here makes that animation grow exponentially on every update.
      sprite_rect(d, s, &l, &t, &rr, &b);
      return lv_num((ln == LN_WIDTH) ? rr - l : b - t);
    }
    dg_bounds(d, (unsigned)lv_id(owner), &l, &t, &rr, &b);
    if (ln == LN_LEFT)
      return lv_num(l);
    if (ln == LN_TOP)
      return lv_num(t);
    if (ln == LN_RIGHT)
      return lv_num(rr);
    if (ln == LN_BOTTOM)
      return lv_num(b);
#if DG_MODERN
    if (ln == LN_RECT)
      return lv_rect(r, l, t, rr, b);
#endif
#if DG_MODERN
    if ((ln == LN_FORECOLOR) || (ln == LN_COLOR))
      return (s->value.flags & 16)
                 ? lv_make(LV_COLOR, (int32_t)s->value.fore_rgb)
                 : lv_num(s->value.fore);
    if (ln == LN_BACKCOLOR)
      return (s->value.flags & 32)
                 ? lv_make(LV_COLOR, (int32_t)s->value.back_rgb)
                 : lv_num(s->value.back);
#else
    if (ln == LN_FORECOLOR)
      return lv_num(s->value.fore);
#endif
#if DG_EXTENDED
    lv_t instance = behavior_property(d, (unsigned)lv_id(owner), name);
    if (lv_type(instance) == LV_INSTANCE) return lv_get(r, NULL, name, instance);
#endif
#if DG_MODERN
  } else if (lv_type(owner) == LV_CASTLIB) {
#if DG_D10
    const dg_cast_t *castlib = d10_castlib(d, lv_id(owner));
    if (!castlib) {
      error(d, "cast library range", name);
      return (lv_t){0};
    }
    if (ln == LN_NAME)
      return lv_text(r, castlib->name, false);
#else
    if (lv_id(owner) < 1 || (unsigned)lv_id(owner) > d->movie->cast_count) {
      error(d, "cast library range", name);
      return (lv_t){0};
    }
    if (ln == LN_NAME)
      return lv_text(r, d->movie->casts[lv_id(owner) - 1].name, false);
#endif
    if (ln == LN_NUMBER)
      return lv_num(lv_id(owner));
  } else if (lv_type(owner) == LV_SOUND) {
    if (lv_id(owner) < 1 || lv_id(owner) > DG_SOUND_CHANNELS) {
      error(d, "audio channel", name);
      return (lv_t){0};
    }
    if (ln == LN_VOLUME)
      return lv_num(d->channel_volume[lv_id(owner) - 1]);
    if (ln == LN_STATUS)
      return lv_num(d->platform.sound_busy &&
                    d->platform.sound_busy(d->context, lv_id(owner) - 1));
    if (ln == LN_ENDTIME) {
      // The playback range end in milliseconds; converted members always
      // play their full sample count (rooms size waits from it).
      const dg_member_t *m = dg_member(d, d->sounds[lv_id(owner) - 1]);
      return lv_num(m && m->rate ? (double)m->samples * 1000.0 / m->rate : 0);
    }
  } else if (lv_type(owner) == LV_XTRA) {
    if (ln == LN_NAME)
      return lv_text(r, lv_id(owner) == 1 ? "FileIO" : "Glu32", false);
#endif
  } else if (lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_FIELD) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
#if DG_MODERN
    if (ln == LN_FONT) {
      int i = field(d, (uint32_t)lv_id(owner));
      return i >= 0 ? lv_text(r, d->field_fonts[i], false) : (lv_t){0};
    }
    if (ln == LN_SCROLLTOP) {
      int i = field(d, (uint32_t)lv_id(owner));
      return lv_num(i >= 0 ? d->field_scroll[i] : 0);
    }
    if (ln == LN_TEXTHEIGHT)
      return lv_num(m && m->text_style ? m->text_style->line_height : 12);
#endif
    if (ln == LN_TEXT) {
#if DG_EXTENDED
      // Hundreds of immutable database rows are read during Willy's startup.
      // Reserve mutable field slots only when source code writes a member.
      for (unsigned i = 0; i < d->field_count; i++)
        if (d->field_members[i] == (uint32_t)lv_id(owner))
          return r->roots[DG_FIELD_TEXT_ROOT + i];
      return m && m->text_bytes ? lv_bytes(r, m->text, m->text_bytes)
             : lv_text(r, m && m->text ? m->text : "", false);
#else
      int i = field(d, (uint32_t)lv_id(owner));
      return i >= 0 ? r->roots[DG_FIELD_TEXT_ROOT + i] : (lv_t){0};
#endif
    }
    if (!m) {
#if DG_MODERN
      if (ln == LN_NUMBER)
        return lv_num((lv_id(owner) & 65535) ? (lv_id(owner) & 65535) : -1);
      if (ln == LN_TYPE)
        return lv_text(r, "empty", true);
      if (ln == LN_NAME)
        return lv_text(r, "", false);
      if ((ln == LN_WIDTH) || (ln == LN_HEIGHT))
        return lv_num(0);
      if (ln == LN_CASTLIBNUM)
        return lv_num(member_cast(d, (uint32_t)lv_id(owner)));
      // An emptied slot has no rect; ScummVM answers VOID rather than alert,
      // and Mucklas's baby game restarts by copying an emptied member's rect.
      if (ln == LN_RECT)
        return (lv_t){0};
#endif
      script_error(d, "missing member", "property");
      return (lv_t){0};
    }
#if DG_MODERN
#if DG_EXTENDED
    if (ln == LN_SIZE) return lv_num(m->source_bytes);
#endif
    if (ln == LN_NAME)
      return lv_text(r, m->name, false);
    if (ln == LN_CASTLIBNUM) {
      return lv_num(member_cast(d, (uint32_t)lv_id(owner)));
    }
    if (ln == LN_TYPE) {
      static const char *const types[] = {
          "empty", "bitmap", "filmloop",   "text",  "palette", "picture",
          "sound", "field",  "shape",      "movie", "video",   "script",
          "rtf",   "ole",    "transition", "xtra"};
#if DG_D10
      // Converted Xtra members answer their authored type; scripts branch on
      // #flash and #vectorShape, never on the converted native representation.
      if (m->source_xtra == 1) return lv_text(r, "flash", true);
      if (m->source_xtra == 2) return lv_text(r, "vectorShape", true);
#endif
      return lv_text(r,
                     m->type < sizeof(types) / sizeof(types[0]) ? types[m->type]
                                                                : "unknown",
                     true);
    }
    if (ln == LN_RECT)
      return lv_rect(r, 0, 0, m->width, m->height);
    if (ln == LN_LOOP)
      return lv_num(member_looping(d, m));
#endif
    if (ln == LN_WIDTH)
      return lv_num(m->width);
    if (ln == LN_HEIGHT)
      return lv_num(m->height);
    if (ln == LN_REGPOINT)
      return lv_point(r, m->reg_x, m->reg_y);
    if ((ln == LN_NUMBER) || (ln == LN_MEMBERNUM))
      return lv_num(m->number);
  }
#if DG_D10
  // Unknown names on a Flash sprite address its internal timeline objects,
  // never a missing runtime property.
  {
    lv_t flash = {0};
    if (d10_flash_get(d, name, owner, &flash))
      return flash;
  }
#endif
  error(d, "unimplemented property getter", name);
  return (lv_t){0};
}
static void set(lv_runtime_t *r, const char *name, lv_t owner, lv_t value) {
  unsigned ln = lv_name_id(r, name);
  dg_runtime_t *d = r->context;
#if DG_EXTENDED
  if (d6_set(d, name, owner, value)) return;
#endif
#if DG_D5
  if(d5_set(d,name,owner,value))return;
#endif
#if DG_D10
  if(d10_set(d,name,owner,value))return;
#endif
  if (lv_type(owner) == LV_VOID) {
#if DG_MODERN
    if (ln == LN_EXITLOCK) {
      d->exit_lock = lv_truth(r, value);
      return;
    }
    if (ln == LN_SEARCHCURRENTFOLDER) {
      d->search_current_folder = lv_truth(r, value);
      return;
    }
    if (ln == LN_SEARCHPATHS) {
      r->roots[0] = value;
      return;
    }
    if (ln == LN_STAGECOLOR) {
      unsigned color = (unsigned)lv_integer(r, value);
      d->stage_color = lv_type(value) == LV_COLOR || color > 255 || !d->movie->palette
                           ? color & 0xffffff
                           : d->movie->palette[color];
      return;
    }
#endif
    if (ln == LN_TIMER) {
      d->timer = d->ticks - (uint32_t)lv_integer(r, value);
      return;
    }
    if (ln == LN_SEARCHPATH) {
      r->roots[0] = value;
      return;
    }
#if DG_EXTENDED
    if ((ln == LN_MOUSEDOWNSCRIPT) || (ln == LN_KEYDOWNSCRIPT)) {
      char *target = (ln == LN_MOUSEDOWNSCRIPT) ? d->mouse_down_script : d->key_down_script;
      if (strlen(lv_cstr(r, value)) >= 64) error(d, "input handler name", "capacity");
      else strcpy(target, lv_cstr(r, value));
      return;
    }
#endif
    if (ln == LN_MOUSEUPSCRIPT) {
      snprintf(d->mouse_up_script, sizeof(d->mouse_up_script), "%s",
               lv_cstr(r, value));
      return;
    }
    if (ln == LN_SOUNDLEVEL) {
      d->sound_level = lv_integer(r, value);
      return;
    }
    if (ln == LN_COLORDEPTH) {
      d->color_depth = lv_integer(r, value);
      return;
    }
    if (ln == LN_FLOATPRECISION) {
      d->float_precision = lv_integer(r, value);
      return;
    }
    if (ln == LN_CURSOR) {
      cursor_set(d, &d->cursor, value);
      return;
    }
    if (ln == LN_MOVIERATE) {
      if (lv_number(r, value) != 1)
        error(d, "unsupported movie rate", "value");
      return;
    }
  } else if (lv_type(owner) == LV_SPRITE) {
    dg_sprite_t *s = sprite(d, owner);
    if (!s)
      return;
    dg_sprite_changed(d, (unsigned)lv_id(owner));
    if (ln == LN_PUPPET) {
      s->puppet = lv_truth(r, value);
      if (!s->puppet) { s->auto_mask = 0; s->moveable = false; }
      return;
    }
    if (ln == LN_CURSOR) {
      cursor_set(d, &s->cursor, value);
      return;
    }
#if DG_CAP_CONSTRAINTS
    if (ln == LN_CONSTRAINT) {
      if (lv_type(value) == LV_MEMBER) {
        for (unsigned i = 1; i < DG_SPRITES; i++)
          if (d->sprites[i].value.member == (uint32_t)lv_id(value)) {
            s->constraint = (int32_t)i;
            break;
          }
      } else {
        s->constraint = lv_integer(r, value);
      }
      return;
    }
#endif
    if ((ln == LN_CASTNUM) || (ln == LN_MEMBERNUM) || (ln == LN_MEMBER)) {
      lv_t library = {0};
#if DG_D7_OR_D10
      // memberNum arithmetic keeps the sprite's current cast library. Explicit
      // multiplexed numbers and member references still select their own cast.
      if ((ln == LN_MEMBERNUM) && lv_type(value) == LV_NUMBER && lv_integer(r,value) < 131072)
        library = lv_num(s->value.member ? member_cast(d, s->value.member) : 1);
#endif
      lv_t ref = reference(r, "member", value, library);
      set_member(s, (uint32_t)lv_id(ref));
#if DG_D5
      d5_media_init(d,s);
#endif
#if DG_MODERN
      event_channel_changed(d, (unsigned)lv_id(owner));
#endif
      s->auto_mask |= DG_MEMBER;
      s->value.type = 16;
      const dg_member_t *m = dg_member(d, s->value.member);
      // Empty cast slots are legal and used by the source to hide sprites.
      if (!m || m->type == 4 || m->type == 6 || m->type == 11 || m->type == 14)
        s->value.type = 0;
#if DG_EXTENDED
      if (m && m->type == 1 && (!m->width || !m->height)) s->value.type = 0;
#endif
      if (m && !s->stretch && m->width) {
        s->value.width = m->width;
        s->value.height = m->height;
      }
      return;
    }
#if DG_MODERN
    if (ln == LN_TYPE) {
      s->value.type = lv_integer(r, value);
      s->auto_mask |= DG_TYPE;
      return;
    }
    if (ln == LN_LOCZ) {
      s->value.loc_z = lv_integer(r, value);
      return;
    }
    if (ln == LN_ROTATION) {
      s->value.rotation = (int32_t)(lv_number(r, value) * 100);
      s->auto_mask |= DG_ROTATION;
      return;
    }
    if (ln == LN_SKEW) {
      s->value.skew = (int32_t)(lv_number(r, value) * 100);
      s->auto_mask |= DG_SKEW;
      return;
    }
    if (ln == LN_INK) {
      s->value.ink = lv_integer(r, value);
      s->auto_mask |= DG_INK;
      return;
    }
    if (ln == LN_FLIPH) {
      s->flip_h = lv_truth(r, value);
      return;
    }
    if (ln == LN_FLIPV) {
      s->flip_v = lv_truth(r, value);
      return;
    }
    if (ln == LN_SCRIPTINSTANCELIST) {
      r->roots[DG_BEHAVIOR_ROOT + lv_id(owner)] = value;
      event_channel_changed(d, (unsigned)lv_id(owner));
      return;
    }
    if (ln == LN_QUAD) {
      if (lv_type(value) != LV_LIST || lv_count(r, value) != 4) {
        error(d, "sprite quad", "requires four points");
        return;
      }
      for (unsigned i = 0; i < 4; i++) {
        lv_t point = lv_at(r, value, i + 1);
        if (lv_type(point) != LV_LIST || lv_count(r, point) != 2) {
          error(d, "sprite quad", "requires point coordinates");
          return;
        }
        s->quad[2 * i] = lv_number(r, lv_at(r, point, 1)) - s->value.x;
        s->quad[2 * i + 1] = lv_number(r, lv_at(r, point, 2)) - s->value.y;
      }
      s->custom_quad = true;
      s->stretch = true;
      s->auto_mask |= DG_SIZE | DG_POSITION | DG_ROTATION | DG_SKEW;
      return;
    }
    if (ln == LN_RECT) {
      // Anything but a rect leaves the sprite as it was (ScummVM ignores it).
      if ((lv_type(value) != LV_LIST && lv_type(value) != LV_PROPLIST) || lv_count(r, value) < 4)
        return;
      s->custom_quad = false;
      int l = lv_integer(r, lv_at(r, value, 1)),
          t = lv_integer(r, lv_at(r, value, 2));
      s->value.width = lv_integer(r, lv_at(r, value, 3)) - l;
      s->value.height = lv_integer(r, lv_at(r, value, 4)) - t;
      s->stretch = true;
      const dg_member_t *m = dg_member(d, s->value.member);
      s->value.x = l + (m && m->width ? m->reg_x * s->value.width / m->width
                                      : s->value.width / 2);
      s->value.y = t + (m && m->height ? m->reg_y * s->value.height / m->height
                                       : s->value.height / 2);
      s->auto_mask |= DG_SIZE | DG_POSITION;
      return;
    }
#endif
    if (ln == LN_VISIBLE) {
      s->visible = lv_truth(r, value);
      return;
    }
    if (ln == LN_STRETCH) {
      s->stretch = lv_truth(r, value);
      return;
    }
    if (ln == LN_TRAILS) {
      s->trails = lv_truth(r, value);
      return;
    }
    if (ln == LN_MOVEABLESPRITE) {
      s->moveable = lv_truth(r, value);
      s->auto_mask |= DG_MOVEABLE;
      return;
    }
    if (ln == LN_LOC) {
      if (lv_type(value) == LV_NUMBER)
        set_position(d, s, lv_integer(r, value), lv_integer(r, value));
      else {
        int x = lv_integer(r, lv_at(r, value, 1));
        int y = lv_integer(r, lv_at(r, value, 2));
        set_position(d, s, x, y);
      }
      s->auto_mask |= DG_POSITION;
      return;
    }
#if DG_EXTENDED
    lv_t instance = behavior_property(d, (unsigned)lv_id(owner), name);
    if (lv_type(instance) == LV_INSTANCE) {lv_set(r, NULL, name, instance, value);return;}
#endif
    int v = lv_integer(r, value);
    if (ln == LN_LOCH) {
      set_position(d, s, v, s->value.y);
      s->auto_mask |= DG_POSITION;
      return;
    }
    if (ln == LN_LOCV) {
      set_position(d, s, s->value.x, v);
      s->auto_mask |= DG_POSITION;
      return;
    }
    if (ln == LN_BLEND) {
      if (v < 0)
        v = 0;
      if (v > 100)
        v = 100;
      s->value.blend = v;
      // Recovered scripts use blend 0 to hide, including copy-ink sprites.
      // Score amount/flags subsequently copy back independently.
      s->value.thickness |= DG_HAS_BLEND;
      return;
    }
    if (ln == LN_WIDTH) {
      s->value.width = v;
      s->stretch = true;
      s->auto_mask |= DG_WIDTH;
      return;
    }
    if (ln == LN_HEIGHT) {
      s->value.height = v;
      s->stretch = true;
      s->auto_mask |= DG_HEIGHT;
      return;
    }
    if (ln == LN_FORECOLOR
#if DG_MODERN
        || ln == LN_COLOR
#endif
    ) {
      s->value.fore = v;
#if DG_MODERN
      s->value.fore_rgb = (uint32_t)v & 0xffffff;
      if (lv_type(value) == LV_COLOR || v > 255)
        s->value.flags |= 16;
      else
        s->value.flags &= ~16;
#endif
      s->auto_mask |= DG_FORE;
      return;
    }
#if DG_MODERN
    if (ln == LN_BACKCOLOR) {
      s->value.back = v;
      s->value.back_rgb = (uint32_t)v & 0xffffff;
      if (lv_type(value) == LV_COLOR || v > 255)
        s->value.flags |= 32;
      else
        s->value.flags &= ~32;
      s->auto_mask |= DG_BACK;
      return;
    }
#endif
    int l, t, rr, b;
    dg_bounds(d, (unsigned)lv_id(owner), &l, &t, &rr, &b);
    if (ln == LN_LEFT) {
      set_position(d, s, s->value.x + v - l, s->value.y);
      s->auto_mask |= DG_POSITION;
      return;
    }
    if (ln == LN_RIGHT) {
      set_position(d, s, s->value.x + v - rr, s->value.y);
      s->auto_mask |= DG_POSITION;
      return;
    }
    if (ln == LN_TOP) {
      set_position(d, s, s->value.x, s->value.y + v - t);
      s->auto_mask |= DG_POSITION;
      return;
    }
    if (ln == LN_BOTTOM) {
      set_position(d, s, s->value.x, s->value.y + v - b);
      s->auto_mask |= DG_POSITION;
      return;
    }
#if DG_MODERN
  } else if (lv_type(owner) == LV_SOUND) {
    if ((ln == LN_VOLUME) && lv_id(owner) >= 1 && lv_id(owner) <= DG_SOUND_CHANNELS) {
      int volume = lv_integer(r, value);
      cancel_fade(d, lv_id(owner) - 1);
      d->channel_volume[lv_id(owner) - 1] = volume < 0     ? 0
                                        : volume > 255 ? 255
                                                       : volume;
      return;
    }
#endif
  } else if (lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_FIELD) {
    // Text/font/color mutations do not change the sprite record itself.
    d->visual_revision++;
#if DG_MODERN
    if (ln == LN_SCROLLTOP) {
      int i = field(d, (uint32_t)lv_id(owner)), scroll = lv_integer(r, value);
      if (i >= 0) d->field_scroll[i] = scroll < 0 ? 0 : scroll;
      return;
    }
    if (ln == LN_FONT) {
      int i = field(d, (uint32_t)lv_id(owner));
      const char *font = lv_cstr(r, value);
      if (strlen(font) >= 64) {
        error(d, "font name capacity", "exceeded");
        return;
      }
      if (i >= 0)
        snprintf(d->field_fonts[i], sizeof(d->field_fonts[i]), "%s", font);
      return;
    }
    if (ln == LN_LOOP) {
      const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
      if (!m || m->type != 6) {
        error(d, "loop property requires sound member", name);
        return;
      }
      unsigned i = 0;
      while (i < d->loop_count && d->loop_members[i] != m->id)
        i++;
      if (i == d->loop_count) {
        if (i >= 64) {
          error(d, "sound member override capacity", "exceeded");
          return;
        }
        d->loop_members[d->loop_count++] = m->id;
      }
      d->loop_values[i] = lv_truth(r, value);
      return;
    }
#endif
    if (ln == LN_PURGEPRIORITY)
      return; // Explicit preload hint: native cache has its own bounded policy.
#if DG_D10
    // Flash playback settings on a converted member. The conversion flattens
    // every Flash asset to a film timeline the score drives, so the exercise
    // stamps' authored settings describe what the port already does.
    if ((ln == LN_STATIC) || (ln == LN_PLAYBACKMODE) || (ln == LN_PAUSEDATSTART) ||
        (ln == LN_CLICKMODE) || (ln == LN_EVENTPASSMODE) || (ln == LN_OBEYSCOREROTATION) ||
        (ln == LN_FIXEDRATE) || (ln == LN_QUALITY) || (ln == LN_SCALEMODE))
      return;
    // Typographic refinements the native text renderer does not apply: it
    // lays out a converted bitmap font at the member's own metrics.
    if ((ln == LN_CHARSPACING) || (ln == LN_FIXEDLINESPACE) || (ln == LN_KERNING) ||
        (ln == LN_ANTIALIAS) || (ln == LN_ANTIALIASTHRESHOLD) || (ln == LN_MARGIN) ||
        (ln == LN_BOXDROPSHADOW) || (ln == LN_DROPSHADOW) || (ln == LN_BOTTOMSPACING))
      return;
    // A converted vector shape ships as rasterized artwork; its outline
    // cannot be rewritten at runtime, so a resized vertex list is dropped.
    if ((ln == LN_VERTEXLIST) || (ln == LN_CLOSED) || (ln == LN_CURVATURE) ||
        (ln == LN_FLASHRECT) || (ln == LN_ORIGINPOINT))
      return;
#endif
#if DG_D10
    // A text member's colour is its foreground; on a converted bitmap or
    // vector member the authored fill and stroke describe artwork the
    // conversion already baked.
    if ((ln == LN_COLOR) || (ln == LN_STROKECOLOR) || (ln == LN_FILLCOLOR) ||
        (ln == LN_BGCOLOR) || (ln == LN_BACKCOLOR) || (ln == LN_STROKEWIDTH)) {
      const dg_member_t *text_member = dg_member(d, (uint32_t)lv_id(owner));
      if (!(ln == LN_COLOR) || !text_member ||
          (text_member->type != 3 && text_member->type != 7))
        return;
    }
#endif
    if ((ln == LN_FORECOLOR) || (ln == LN_COLOR)) {
      int i = field(d, (uint32_t)lv_id(owner));
      if (i >= 0)
        r->roots[DG_FIELD_COLOR_ROOT + i] = value;
      return;
    }
    if (ln == LN_TEXT) {
      int i = field(d, (uint32_t)lv_id(owner));
      if (i >= 0) {
        // TextCastMember::setField converts with Datum::asString. FILMSPIEL
        // assigns its numeric speed directly; the renderer needs stored text.
        if (lv_type(value) == LV_VOID)
          value = lv_text(r, "", false);
        else if (lv_type(value) != LV_STRING) {
          if (!lv_format(r, value, r->text_scratch, sizeof(r->text_scratch))) {
            error(d, "field text conversion exceeds capacity", name);
            return;
          }
          value = lv_text(r, r->text_scratch, false);
        }
        r->roots[DG_FIELD_TEXT_ROOT + i] = value;
#if DG_MODERN
        d->field_text_changed[i] = true;
#endif
      }
      return;
    }
  }
#if DG_D10
  // Styling a chunk of a text member: a chunk expression evaluates to the
  // text itself, and the native renderer draws a member in one style, so
  // per-character colour and spacing have nowhere to land.
  if ((lv_type(owner) == LV_STRING || lv_type(owner) == LV_SYMBOL) &&
      ((ln == LN_COLOR) || (ln == LN_FORECOLOR) || (ln == LN_CHARSPACING) || (ln == LN_FONT) ||
       (ln == LN_FONTSIZE) || (ln == LN_FONTSTYLE) || (ln == LN_BGCOLOR)))
    return;
#endif
  error(d, "unimplemented property setter", name);
}
static const char *basename_(const char *path) {
  const char *base = path;
  for (; *path; path++)
    if (*path == '/' || *path == '\\' || *path == ':')
      base = path + 1;
  return base;
}
static unsigned frame_number(dg_runtime_t *d, lv_t value) {
  if (lv_type(value) == LV_NUMBER)
    return (unsigned)lv_integer(d->values, value);
  const char *name = lv_cstr(d->values, value);
  if (lv_type(value) == LV_SYMBOL &&
      (eq(name, "next") || eq(name, "previous") || eq(name, "loop"))) {
    unsigned current = 1, last = 1, next = 0, previous = 1;
    for (unsigned i = 0; i < d->movie->label_count; i++) {
      unsigned f = d->movie->labels[i].frame;
      if (f > last)
        last = f;
      if (f <= d->frame && f > current)
        current = f;
      if (f > d->frame && (!next || f < next))
        next = f;
    }
    for (unsigned i = 0; i < d->movie->label_count; i++) {
      unsigned f = d->movie->labels[i].frame;
      if (f < current && f > previous)
        previous = f;
    }
    return eq(name, "next")   ? (next ? next : last)
           : eq(name, "loop") ? current
                              : previous;
  }
  if (eq(name, "loop")) {
    // A real label named Loop wins over the built-in marker-relative name.
    for (unsigned i = 0; i < d->movie->label_count; i++)
      if (eq(d->movie->labels[i].name, name))
        return d->movie->labels[i].frame;
  }
  for (unsigned i = 0; i < d->movie->label_count; i++)
    if (eq(d->movie->labels[i].name, name))
      return d->movie->labels[i].frame;
  script_error(d, "unknown frame label", name);
  return 0;
}
static bool file_call(dg_runtime_t *d, const char *name, unsigned argc,
                      const lv_t *a, lv_t *out) {
  lv_runtime_t *r = d->values;
  if (!strcmp(name, "fileio")) {
    if (argc != 3 || !eq(lv_cstr(r, a[0]), "mnew")) {
      error(d, "invalid FileIO", "constructor");
      return true;
    }
    const char *mode = lv_cstr(r, a[1]), *file = basename_(lv_cstr(r, a[2]));
    if (strlen(file) >= 32) {
      error(d, "invalid save filename", file);
      return true;
    }
    for (unsigned i = 0; i < 4; i++)
      if (!d->files[i].used) {
        dg_file_t *f = &d->files[i];
        memset(f, 0, sizeof(*f));
        snprintf(d->file_names[i], 32, "%s", file);
        f->write = eq(mode, "write") || eq(mode, "append");
        f->append = eq(mode, "append");
        if (!f->write &&
            (!d->platform.read_file ||
             !d->platform.read_file(d->context, file, f->data, sizeof(f->data),
                                    &f->length)))
          return true;
        // Append creates an empty file when none exists, as the FileIO XObject
        // does. Existing content is read before the append position is chosen.
        if (f->append && d->platform.read_file)
          d->platform.read_file(d->context, file, f->data, sizeof(f->data), &f->length);
        f->used = true;
        f->position = f->append ? f->length : 0;
        *out = lv_make(LV_FILE, (int32_t)i);
        return true;
      }
    error(d, "save handle capacity", "exceeded");
    return true;
  }
  if (strcmp(name, "file_method"))
    return false;
  if (argc < 2 || lv_id(a[0]) < 0 || lv_id(a[0]) >= 4 || !d->files[lv_id(a[0])].used) {
    error(d, "invalid FileIO", "handle");
    return true;
  }
  dg_file_t *f = &d->files[lv_id(a[0])];
  const char *method = lv_cstr(r, a[1]);
  if (eq(method, "mdispose")) {
    if (f->write && (!d->platform.write_file ||
                     !d->platform.write_file(d->context, d->file_names[lv_id(a[0])],
                                             f->data, f->length))) {
      *out = lv_text(r, "FAIL", true);
      error(d, "save write failed", d->file_names[lv_id(a[0])]);
    }
    f->used = false;
    return true;
  }
  if (eq(method, "mreadline") || eq(method, "mreadfile")) {
    unsigned start = f->position;
    if (eq(method, "mreadfile"))
      f->position = f->length;
    else
      while (f->position < f->length && f->data[f->position] != '\r' &&
             f->data[f->position] != '\n')
        f->position++;
    unsigned end = f->position;
    char saved = f->data[end];
    f->data[end] = 0;
    *out = lv_text(r, f->data + start, false);
    f->data[end] = saved;
    if (f->position < f->length && f->data[f->position] == '\r')
      f->position++;
    if (f->position < f->length && f->data[f->position] == '\n')
      f->position++;
    return true;
  }
  if (eq(method, "mwritestring") && argc == 3 && f->write) {
    const char *text = lv_cstr(r, a[2]);
    size_t n = strlen(text);
    if (n >= sizeof(f->data) - f->position) {
      error(d, "save file capacity", "exceeded");
      return true;
    }
    memcpy(f->data + f->position, text, n + 1);
    f->position += (unsigned)n;
    f->length = f->position;
    return true;
  }
  error(d, "unsupported FileIO method", method);
  return true;
}
#if DG_MODERN
// Native replacements for the selected game's documented FileIO and display
// helper calls. All file names are resolved by the game save adapter.
static bool d8_file_call(dg_runtime_t *d, const char *name, unsigned argc,
                         const lv_t *a, lv_t *out) {
  lv_runtime_t *r = d->values;
  if (!argc || lv_type(a[0]) != LV_FILE)
    return false;
  if (lv_id(a[0]) < 0 || lv_id(a[0]) >= 4 || !d->files[lv_id(a[0])].used) {
    error(d, "invalid FileIO", "handle");
    return true;
  }
  dg_file_t *f = &d->files[lv_id(a[0])];
  char *filename = d->file_names[lv_id(a[0])];
#define METHOD(s) eq(name, s)
  if (METHOD("status")) {
    *out = lv_num(f->status);
    return true;
  }
  if (METHOD("error")) {
    *out = lv_text(r, f->status ? "File unavailable" : "", false);
    return true;
  }
  if (METHOD("openfile") || METHOD("createfile")) {
    if (argc < 2) {
      error(d, "FileIO filename", name);
      return true;
    }
    const char *base = basename_(lv_cstr(r, a[1]));
    if (!*base || strlen(base) >= 32) {
      f->status = -37;
      return true;
    }
    snprintf(filename, 32, "%s", base);
    f->position = f->length = 0;
    f->dirty = false;
    if (METHOD("createfile")) {
      f->status = d->platform.write_file &&
                          d->platform.write_file(d->context, filename, "", 0)
                      ? 0
                      : -37;
      f->opened = false;
    } else {
      f->opened = d->platform.read_file &&
                  d->platform.read_file(d->context, filename, f->data,
                                        sizeof(f->data) - 1, &f->length);
      f->status = f->opened ? 0 : -37;
      f->write = argc < 3 || lv_integer(r, a[2]) != 1;
      f->data[f->length] = 0;
    }
    *out = lv_num(f->status);
    return true;
  }
  if (METHOD("closefile") || METHOD("dispose")) {
    if (f->dirty &&
        (!d->platform.write_file ||
         !d->platform.write_file(d->context, filename, f->data, f->length)))
      f->status = -37;
    f->dirty = false;
    f->opened = false;
    if (METHOD("dispose"))
      f->used = false;
    *out = lv_num(f->status);
    return true;
  }
  if (METHOD("delete")) {
    f->status = d->platform.write_file &&
                        d->platform.write_file(d->context, filename, "", 0)
                    ? 0
                    : -37;
    f->opened = f->dirty = false;
    f->length = f->position = 0;
    *out = lv_num(f->status);
    return true;
  }
  if (METHOD("getlength")) {
    *out = lv_num(f->opened ? f->length : 0);
    return true;
  }
  if (METHOD("getposition")) {
    *out = lv_num(f->position);
    return true;
  }
  if (METHOD("setposition")) {
    int position = argc > 1 ? lv_integer(r, a[1]) : -1;
    if (position < 0 || (unsigned)position > f->length)
      f->status = -37;
    else
      f->position = position;
    *out = lv_num(f->status);
    return true;
  }
  if (METHOD("readfile") || METHOD("readline") || METHOD("readword")) {
    if (!f->opened) {
      *out = lv_text(r, "", false);
      return true;
    }
    unsigned begin = f->position;
    while (f->position < f->length) {
      char ch = f->data[f->position];
      if (!METHOD("readfile") &&
          (ch == '\r' || ch == '\n' || (METHOD("readword") && ch == ' ')))
        break;
      f->position++;
    }
    unsigned end = f->position;
    char saved = f->data[end];
    f->data[end] = 0;
    *out = lv_text(r, f->data + begin, false);
    f->data[end] = saved;
    if (f->position < f->length && f->data[f->position] == '\r')
      f->position++;
    if (f->position < f->length &&
        (f->data[f->position] == '\n' ||
         (METHOD("readword") && f->data[f->position] == ' ')))
      f->position++;
    return true;
  }
  if (METHOD("writestring")) {
    if (argc < 2 || !f->write) {
      f->status = -37;
      return true;
    }
    const char *text = lv_cstr(r, a[1]);
    size_t length = strlen(text);
    if (length >= sizeof(f->data) - f->position) {
      error(d, "save file capacity", "exceeded");
      return true;
    }
    memcpy(f->data + f->position, text, length);
    f->position += length;
    if (f->length < f->position)
      f->length = f->position;
    f->data[f->length] = 0;
    f->dirty = true;
    f->opened = true;
    f->status = 0;
    *out = lv_num(0);
    return true;
  }
#undef METHOD
  return false;
}
#endif
#if DG_EXTENDED
// Keep sendSprite on the native continuation stack: a behavior may execute go
// and must resume before the sending handler continues. Locals root the original
// attachment list even if the target frame replaces its sprites.
static lv_flow_t send_sprite_step(lv_runtime_t *r, lv_frame_t *f) {
  dg_runtime_t *d = r->context;
  unsigned count = lv_type(f->locals[0]) == LV_LIST ? lv_count(r, f->locals[0]) : 0;
  const char *event = lv_cstr(r, f->locals[1]);
  while (f->pc < count) {
    lv_t instance = lv_at(r, f->locals[0], ++f->pc);
    d->current_event_sprite = (unsigned)lv_integer(r, f->locals[2]);
    if (lv_start_method(r, instance, event,
                        (unsigned)lv_integer(r, f->locals[4]), f->locals + 6)) {
      f->locals[5] = lv_num(1);
      return LV_CALL;
    }
  }
  d->current_event_sprite = (unsigned)lv_integer(r, f->locals[3]);
  if (!lv_truth(r, f->locals[5])) {
    f->locals[5] = lv_num(1);
    for (unsigned i = 0; i < d->loaded_count; i++)
      if (lv_start(r, d->loaded[i]->code, event, 0,
                   (unsigned)lv_integer(r, f->locals[4]), f->locals + 6))
        return LV_CALL;
  }
  return LV_RETURN;
}
static lv_flow_t send_all_step(lv_runtime_t *r, lv_frame_t *f) {
  if (++f->pc >= DG_SPRITES) return LV_RETURN;
  lv_t args[LV_LOCALS] = {lv_num(f->pc), f->locals[0]};
  unsigned count = (unsigned)lv_integer(r, f->locals[1]);
  memcpy(args + 2, f->locals + 2, count * sizeof(lv_t));
  return lv_invoke(r, f, "sendsprite", count + 2, args);
}
static const lv_handler_t send_all_handler = {
  .name = "dispatch", .kind = "MovieScript", .arguments = LV_LOCALS,
  .locals = LV_LOCALS, .step = send_all_step
};
static const lv_movie_t send_all_movie = {"<sendAllSprites>", 1, &send_all_handler,NULL, NULL, NULL, NULL, 0};
static const lv_handler_t send_sprite_handler = {
  .name = "dispatch", .kind = "MovieScript", .arguments = LV_LOCALS,
  .locals = LV_LOCALS, .step = send_sprite_step
};
static const lv_movie_t send_sprite_movie = {"<sendSprite>", 1, &send_sprite_handler,NULL, NULL, NULL, NULL, 0};
#endif
static bool call(lv_runtime_t *r, const char *name, unsigned argc,
                 const lv_t *a, lv_t *out, bool *yield) {
  unsigned ln = lv_name_id(r, name);
  dg_runtime_t *d = r->context;
#if DG_D10
  // Window services take precedence over the D6 file-chooser guard.
  if(d10_call(d,name,argc,a,out))return true;
#endif
#if DG_EXTENDED
  if (d6_call(d, name, argc, a, out)) return true;
#endif
#if DG_D5
  if(d5_call(d,name,argc,a,out))return true;
#endif
#define N(i) lv_integer(r, a[i])
#define ARITY(n)                                                               \
  do {                                                                         \
    if (argc != (n)) {                                                         \
      error(d, "argument count", name);                                        \
      return true;                                                             \
    }                                                                          \
  } while (0)
#if DG_MODERN
  if (d8_file_call(d, name, argc, a, out))
    return true;
  if (ln == LN_SCROLLBYLINE) {
    ARITY(2);
    d->visual_revision++;
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(a[0]));
    int i = field(d, (uint32_t)lv_id(a[0]));
    int line = m && m->text_style ? m->text_style->line_height : 12;
    if (i >= 0) {
      int64_t scroll = d->field_scroll[i] + (int64_t)N(1) * line;
      d->field_scroll[i] = scroll < 0 ? 0 : scroll > 65535 ? 65535 : (int)scroll;
    }
    return true;
  }
#if DG_CAP_KEYBOARD
  if (ln == LN_KEYPRESSED) {
    ARITY(1);
    bool down = false;
    if (lv_type(a[0]) == LV_NUMBER) {
      int code = N(0);
      down = code >= 0 && code < 128 &&
             (d->keys_down[code >> 5] & (UINT32_C(1) << (code & 31)));
    } else if (lv_type(a[0]) == LV_STRING) {
      const unsigned char *key = (const unsigned char *)lv_cstr(r, a[0]);
      if (key[0] && !key[1])
        for (unsigned code = 0; code < 128; code++)
          if ((d->keys_down[code >> 5] & (UINT32_C(1) << (code & 31))) &&
              tolower(d->key_chars[code]) == tolower(key[0]))
            down = true;
    } else {
      error(d, "keyPressed argument", "requires a character or key code");
    }
    *out = lv_num(down);
    return true;
  }
#endif
  if (ln == LN_CASTLIB) {
    ARITY(1);
    *out = reference(r, "castlib", a[0], (lv_t){0});
    return true;
  }
#if DG_EXTENDED
  if (ln == LN_SENDALLSPRITES) {
    if (!argc || argc > LV_LOCALS - 5) {error(d, "sendAllSprites arguments", "capacity");return true;}
    lv_t args[LV_LOCALS] = {a[0], lv_num(argc - 1)};
    memcpy(args + 2, a + 1, (argc - 1) * sizeof(lv_t));
    lv_start(r, &send_all_movie, "dispatch", 0, argc + 1, args);
    return true;
  }
#endif
  if (ln == LN_SENDSPRITE) {
    if (argc < 2 || argc > LV_LOCALS) {
      error(d, "sendSprite arguments", name);
      return true;
    }
    // Channel zero is legal (no attached behavior, with movie fallback).
    // A nonexistent channel has no receiver and returns VOID, as b_sendSprite.
#if DG_D10
    // Dynamic sprite tells forward their sprite value as the receiver.
    const long send_channel = lv_type(a[0]) == LV_SPRITE ? lv_id(a[0]) : N(0);
#else
    const long send_channel = N(0);
#endif
    if (send_channel < 0 || send_channel >= DG_SPRITES) { *out = (lv_t){0}; return true; }
    char event[80];
    const char *source = lv_cstr(r, a[1]);
    if (strlen(source) >= sizeof(event)) {
      error(d, "sendSprite event capacity", name);
      return true;
    }
    unsigned length = (unsigned)strlen(source);
    for (unsigned i = 0; i <= length; i++)
      event[i] = (char)tolower((unsigned char)source[i]);
    unsigned channel = (unsigned)send_channel, previous = d->current_event_sprite;
    lv_t list = r->roots[DG_BEHAVIOR_ROOT + channel];
#if DG_D10
    // sendSprite(0, …) addresses the frame behavior. Where the span attached
    // instances the ordinary path below reaches them; a frame script with no
    // behavior attached still answers on its cast script, before any movie
    // fallback.
    if (!channel && (lv_type(list) != LV_LIST || !lv_count(r, list)) &&
        d->score[0].script) {
      uint32_t frame_member = d->score[0].script;
      const dg_movie_t *frame_movie = dg_loaded(d, frame_member >> 20);
      unsigned frame_cast = (frame_member >> 16) & 15;
      if (frame_movie && frame_cast && frame_cast <= frame_movie->cast_count) {
        d->current_event_sprite = 0;
        if (lv_start_cast_args(r, frame_movie->code, event,
                               frame_member & 65535,
                               frame_movie->casts[frame_cast - 1].name,
                               argc - 2, a + 2)) {
          *out = (lv_t){0};
          return true;
        }
        d->current_event_sprite = previous;
      }
    }
#endif
#if DG_EXTENDED
    if (argc > LV_LOCALS - 4) {
      error(d, "sendSprite arguments", "continuation capacity");
      return true;
    }
    lv_t args[LV_LOCALS] = {list, a[1], lv_num(channel), lv_num(previous),
                           lv_num(argc - 2), lv_num(r->depth && r->frames[r->depth - 1].movie == &send_all_movie)};
    memcpy(args + 6, a + 2, (argc - 2) * sizeof(lv_t));
    lv_start(r, &send_sprite_movie, "dispatch", 0, argc + 4, args);
    return true;
#else
    bool handled = false;
    // sendSprite calls every attached behavior synchronously, as in D8. The
    // recovered LO/RS handlers do not yield; lv_call_method enforces its bound.
    unsigned count = lv_type(list) == LV_LIST ? lv_count(r, list) : 0;
    for (unsigned i = 1; i <= count && !r->failed; i++) {
      lv_t instance = lv_at(r, list, i);
      lv_t script = lv_type(instance) == LV_INSTANCE
                        ? lv_get(r, NULL, "script", instance) : instance;
      const lv_movie_t *owner = NULL;
      if (!resolve_script(r, script, event, &owner))
        continue;
      d->current_event_sprite = channel;
      *out = lv_call_method(r, r->depth ? &r->frames[r->depth - 1] : NULL,
                             event, instance, argc - 2, a + 2);
      handled = true;
    }
    d->current_event_sprite = previous;
    if (!handled) {
      const lv_movie_t *owner = d->movie->code;
      const lv_handler_t *h = lv_find(r, owner, event, 0);
      for (unsigned i = 1; !h && i < d->loaded_count; i++) {
        owner = d->loaded[i]->code;
        h = lv_find(r, owner, event, 0);
      }
      if (h) {
        lv_frame_t caller = {.movie = owner};
        *out = lv_call(r, &caller, event, argc - 2, a + 2);
      }
    }
    return true;
#endif
  }
  if ((ln == LN_PRELOAD) && argc && lv_type(a[0]) == LV_MEMBER) {
    *out = lv_num(0);
    return true;
  }
  if (ln == LN_SOUND_FADEOUT) {
    if (argc < 1 || argc > 2) {
      error(d, "audio fade arguments", name);
      return true;
    }
    int channel = N(0),
        duration = argc > 1 ? N(1) : (int)(900 / (d->tempo ? d->tempo : 30));
    if (channel < 1 || channel > DG_SOUND_CHANNELS || duration < 0) {
      error(d, "audio fade range", name);
      return true;
    }
    unsigned ch = channel - 1;
    cancel_fade(d, ch);
    d->fade_start[ch] = d->ticks;
    d->fade_duration[ch] = duration;
    d->fade_volume[ch] = d->channel_volume[ch];
    if (!duration)
      d->channel_volume[ch] = 0;
    return true;
  }
  if (ln == LN_SOUND) {
    ARITY(1);
    int channel = N(0);
    if (channel < 1 || channel > DG_SOUND_CHANNELS)
      error(d, "audio channel", name);
    *out = lv_make(LV_SOUND, channel);
    return true;
  }
  if ((ln == LN_STOP) && argc == 1 && lv_type(a[0]) == LV_SOUND) {
    unsigned ch = (unsigned)lv_id(a[0]) - 1;
    if (ch >= DG_SOUND_CHANNELS) {
      error(d, "audio channel", name);
      return true;
    }
    cancel_fade(d, ch);
    d->sounds[ch] = 0;
    d->sound_puppet[ch] = false;
    d->sound_serial[ch]++;
    if (d->platform.sound)
      d->platform.sound(d->context, ch, NULL);
    return true;
  }
  if (ln == LN_XTRA) {
    ARITY(1);
    const char *xtra = lv_type(a[0]) == LV_NUMBER ? "" : lv_cstr(r, a[0]);
    int id = lv_type(a[0]) == LV_NUMBER ? lv_integer(r, a[0])
             : eq(xtra, "fileio")   ? 1
             : eq(xtra, "glu32")    ? 2
                                    : 0;
    if (id < 1 || id > 2)
      id = 0;
    if (!id) {
#if DG_D10
      // Authored code probes optional Xtras and tolerates their absence
      // through objectp guards; answer VOID instead of failing.
      char text[96];
      snprintf(text, sizeof(text), "XTRA_UNAVAILABLE %.64s", xtra);
      if (d->platform.trace) d->platform.trace(d->context, text);
      *out = (lv_t){0};
      return true;
#else
      error(d, "unsupported Xtra", xtra);
#endif
    }
    *out = lv_make(LV_XTRA, id);
    return true;
  }
  if ((ln == LN_NEW) && argc == 1 && lv_type(a[0]) == LV_XTRA) {
    if (lv_id(a[0]) == 2) {
      *out = a[0];
      return true;
    }
    if (lv_id(a[0]) == 1) {
      for (unsigned i = 0; i < 4; i++)
        if (d->files[i].used && !d->files[i].opened && !d->files[i].dirty &&
            !lv_has_reference(r, LV_FILE, (int32_t)i))
          d->files[i].used = false;
      for (unsigned i = 0; i < 4; i++)
        if (!d->files[i].used) {
          memset(&d->files[i], 0, sizeof(d->files[i]));
          d->files[i].used = true;
          *out = lv_make(LV_FILE, (int32_t)i);
          return true;
        }
      error(d, "save handle capacity", "exceeded");
      return true;
    }
  }
  if (ln == LN_GLUREGISTER) {
    ARITY(1);
    *out = lv_num(0);
    return true;
  }
  if (ln == LN_GLUNEW) {
    ARITY(7);
    if (lv_type(a[0]) != LV_XTRA || lv_id(a[0]) != 2 ||
        !eq(lv_cstr(r, a[2]), "initdisplay"))
      error(d, "unsupported GLU entry", name);
    *out = lv_num(0);
    return true;
  }
  if ((ln == LN_GLUCALL) || (ln == LN_GLUGETLASTERROR)) {
    ARITY(1);
    *out = lv_num(0);
    return true;
  }
  if (ln == LN_GLUGETERRORSTRING) {
    ARITY(2);
    *out = lv_text(r, "", false);
    return true;
  }
  if (ln == LN_BADISKINFO) {
    ARITY(2);
    if (!eq(lv_cstr(r, a[1]), "type"))
      error(d, "unsupported disk information", name);
    *out = lv_text(r, eq(lv_cstr(r, a[0]), "d") ? "CD-ROM" : "Unknown", false);
    return true;
  }
  if (ln == LN_BASETDISPLAY) {
    *out = lv_num(1);
    return true;
  }
  if (ln == LN_ALERT) {
    ARITY(1);
    if (d->platform.trace)
      d->platform.trace(d->context, lv_cstr(r, a[0]));
    return true;
  }
#if DG_D10
  // An authoring-time diagnostic: the shipped projector had no debug Xtra,
  // so the message reached nobody. It stays a trace, never a stage dialog.
  if ((ln == LN_DEBUGALERT) && argc == 1) {
    if (d->platform.trace) {
      char text[192];
      snprintf(text, sizeof(text), "DEBUG_ALERT %s", lv_cstr(r, a[0]));
      d->platform.trace(d->context, text);
    }
    return true;
  }
#endif
#endif
  if (file_call(d, name, argc, a, out))
    return true;
  if (ln == LN_STARTTIMER) {
    ARITY(0);
    d->timer = d->ticks;
    return true;
  }
  if (ln == LN_UPDATESTAGE) {
    ARITY(0);
    // Director 6 updates the stage synchronously; it does not wait a timer
    // tick per object. The platform displays the latest stage when ready.
    dg_update_stage(d);
    return true;
  }
  if (ln == LN_DELAY) {
    ARITY(1);
    int delay = N(0);
    if (delay < 0) {
      error(d, "negative delay", name);
      return true;
    }
    d->resume_tick = d->ticks + (unsigned)delay;
    *yield = true;
    return true;
  }
#if DG_EXTENDED
  if (ln == LN_MARKER) {
    ARITY(1);
    if (lv_type(a[0]) == LV_STRING || lv_type(a[0]) == LV_SYMBOL) {
#if DG_D10
      // Authored existence probes branch on marker(name); an absent label
      // answers zero under Director MX, never an alert.
      const char *label = lv_cstr(r, a[0]);
      for (unsigned i = 0; i < d->movie->label_count; i++)
        if (eq(d->movie->labels[i].name, label)) {
          *out = lv_num(d->movie->labels[i].frame);
          return true;
        }
      *out = lv_num(0);
      return true;
#else
      *out = lv_num(frame_number(d, a[0])); return true;
#endif
    }
    int index = -1, count = (int)d->movie->label_count, delta = N(0);
    for (int i = 0; i < count; i++)
      if (d->movie->labels[i].frame <= d->frame) index = i;
    if (!count || (index < 0 && !delta)) {*out = lv_num(0);return true;}
    int64_t target = (int64_t)index + delta;
    target = target < 0 ? 0 : target >= count ? count - 1 : target;
    *out = lv_num(d->movie->labels[target].frame); return true;
  }
#endif
#if DG_D10
  if ((ln == LN_GOTOMOVIE) && argc == 1) {
    if (d->tell_window) {
      d10_window_request(d, lv_cstr(r, a[0]));
      return true;
    }
    snprintf(d->next_movie, sizeof(d->next_movie), "%s", basename_(lv_cstr(r, a[0])));
    char *dot = strrchr(d->next_movie, '.');
    if (dot && eq(dot, ".dir")) memcpy(dot, ".DXR", 5);
    // The corpus navigates by bare movie stems; registered names carry .DXR.
    if (!dot && strlen(d->next_movie) + 4 < sizeof(d->next_movie))
      strcat(d->next_movie, ".DXR");
    d->next_movie_label[0] = 0;
    d->next_movie_frame = 1;
    *yield = true;
    return true;
  }
#endif
  if (ln == LN_GO) {
    if (argc < 1 || argc > 2) {
      error(d, "go arguments", name);
      return true;
    }
    if (argc == 2) {
      snprintf(d->next_movie, sizeof(d->next_movie), "%s",
               basename_(lv_cstr(r, a[1])));
      char *extension = strrchr(d->next_movie, '.');
      if (extension && eq(extension, ".dir")) memcpy(extension, ".DXR", 5);
#if DG_MODERN
      d->next_movie_label[0]=0;
      if(lv_type(a[0])==LV_STRING || lv_type(a[0])==LV_SYMBOL) {
        snprintf(d->next_movie_label,sizeof(d->next_movie_label),"%s",lv_cstr(r,a[0]));
        d->next_movie_frame=1;
      } else
#endif
      d->next_movie_frame = (unsigned)N(0);
    } else
      d->next_frame = frame_number(d, a[0]);
    *yield = true;
    return true;
  }
  if (ln == LN_PLAY_MOVIE) {
    ARITY(1);
    error(d, "nested movie needs continuation stack", lv_cstr(r, a[0]));
    return true;
  }
  if (ln == LN_PLAY_DONE) {
    ARITY(0);
    error(d, "play done without movie stack", name);
    return true;
  }
  if (ln == LN_QUIT) {
    d->quit = true;
    return true;
  }
  if (ln == LN_CURSOR) {
    ARITY(1);
    cursor_set(d, &d->cursor, a[0]);
    return true;
  }
  if (ln == LN_PUPPETTEMPO) {
    ARITY(1);
    int tempo = N(0);
    if (tempo < 0 || tempo > DG_MAX_TEMPO) {
      error(d, "unsupported puppet tempo", name);
      return true;
    }
    d->puppet_tempo = (unsigned)tempo;
    return true;
  }
  if (ln == LN_PUPPETSPRITE) {
    ARITY(2);
    int index = N(0);
    dg_sprite_t *s = sprite(d, lv_make(LV_SPRITE, index));
    if (s) {
      dg_sprite_changed(d, (unsigned)index);
      bool old = s->puppet;
      s->puppet = lv_truth(r, a[1]);
      if (!s->puppet) {
        s->auto_mask = 0;
        if (old && index > 0)
          apply(s, &d->score[index + 5], DG_ALL);
      }
#if DG_MODERN
      event_channel_changed(d, (unsigned)index);
#endif
    }
    return true;
  }
  if (ln == LN_ROLLOVER) {
    ARITY(1);
    int index = N(0), l, t, rr, b;
    if (index == 0) {
      *out = lv_num(dg_hit(d, d->mouse_x, d->mouse_y));
      return true;
    }
    dg_bounds(d, (unsigned)index, &l, &t, &rr, &b);
    *out = lv_num(d->mouse_x >= l && d->mouse_x < rr && d->mouse_y >= t &&
                  d->mouse_y < b);
    return true;
  }
  if (ln == LN_SOUNDBUSY) {
    ARITY(1);
    int ch = N(0);
    if (ch < 1 || ch > DG_SOUND_CHANNELS) {
      error(d, "audio channel", name);
      return true;
    }
    *out = lv_num(d->platform.sound_busy &&
                  d->platform.sound_busy(d->context, (unsigned)ch - 1));
    return true;
  }
  if ((ln == LN_PUPPETSOUND) || (ln == LN_SOUND_STOP)) {
    if (argc < 1 || argc > 2) {
      error(d, "audio arguments", name);
      return true;
    }
    int ch = (argc == 2 || (ln == LN_SOUND_STOP)) ? N(0) : 1;
    if (ch < 1 || ch > DG_SOUND_CHANNELS) {
      error(d, "audio channel", name);
      return true;
    }
    uint32_t id = 0;
    if (!(ln == LN_SOUND_STOP)) {
      lv_t ref = reference(r, "member", a[argc - 1], (lv_t){0});
      id = (uint32_t)lv_id(ref);
    }
    const dg_member_t *m = dg_member(d, id);
    if (id && (!m || m->type != 6)) {
      // Director ignores references to empty/non-sound cast slots. The disc
      // contains live examples (MALAR's music toggle, BRODER's padded result).
      // Missing converted files for an actual sound remain fatal in the
      // adapter.
      if (d->platform.trace) {
        char text[80];
        snprintf(text, sizeof(text), "EMPTY_SOUND_REFERENCE member=%lu",
                 (unsigned long)id);
        d->platform.trace(d->context, text);
      }
      return true;
    }
#if DG_MODERN
    cancel_fade(d, ch - 1);
#endif
    d->sounds[ch - 1] = id;
    d->sound_serial[ch - 1]++;
    d->sound_puppet[ch - 1] = id != 0;
    play_member(d, (unsigned)ch - 1, m);
    return true;
  }
  if (ln == LN_SPRITEBOX) {
    ARITY(5);
    int index = N(0);
    dg_sprite_t *s = sprite(d, lv_make(LV_SPRITE, index));
    if (s) {
      dg_sprite_changed(d, (unsigned)index);
      int l = N(1), t = N(2), rr = N(3), b = N(4);
      s->value.width = rr - l;
      s->value.height = b - t;
      s->stretch = true;
      s->auto_mask |= DG_SIZE | DG_POSITION;
      const dg_member_t *m = dg_member(d, s->value.member);
      s->value.x = l + (m && m->width ? m->reg_x * s->value.width / m->width
                                      : s->value.width / 2);
      s->value.y = t + (m && m->height ? m->reg_y * s->value.height / m->height
                                       : s->value.height / 2);
    }
    return true;
  }
  if ((ln == LN_WITHIN) || (ln == LN_INTERSECTS)) {
    ARITY(2);
    int al, at, ar, ab, bl, bt, br, bb;
    if (lv_type(a[0]) == LV_LIST) {
      al = ar = lv_integer(r, lv_at(r, a[0], 1));
      at = ab = lv_integer(r, lv_at(r, a[0], 2));
    } else
      dg_bounds(d, (unsigned)N(0), &al, &at, &ar, &ab);
    if (lv_type(a[1]) == LV_LIST) {
      bl = lv_integer(r, lv_at(r, a[1], 1));
      bt = lv_integer(r, lv_at(r, a[1], 2));
      br = lv_integer(r, lv_at(r, a[1], 3));
      bb = lv_integer(r, lv_at(r, a[1], 4));
    } else
      dg_bounds(d, (unsigned)N(1), &bl, &bt, &br, &bb);
    *out = lv_num((ln == LN_WITHIN) ? al >= bl && at >= bt && ar <= br && ab <= bb
                               : al < br && ar > bl && at < bb && ab > bt);
    return true;
  }
  if (ln == LN_GETNTHFILENAMEINFOLDER) {
    ARITY(2);
    const char *name =
        d->platform.nth_file
            ? d->platform.nth_file(d->context, lv_cstr(r, a[0]), N(1))
            : "";
    *out = lv_text(r, name ? name : "", false);
    return true;
  }
  if (ln == LN_OPENXLIB) {
    ARITY(1);
    if (!eq(basename_(lv_cstr(r, a[0])), "fileio"))
      error(d, "unsupported external library", lv_cstr(r, a[0]));
    return true;
  }
  if (ln == LN_UNLOAD) {
    if (argc > 2) error(d, "unload arguments", "range");
    return true; // Frame cache hint; ROM media uses the bounded native cache.
  }
  if ((ln == LN_PRELOADCAST) || (ln == LN_PRELOADMEMBER))
    return true; // Bounded asset-cache policy replaces memory hints.
  if (ln == LN_NOTHING) {
    ARITY(0);
    return true;
  }
  if (ln == LN_BEEP) {
    if (d->platform.trace)
      d->platform.trace(d->context, "BEEP");
    return true;
  }
  if (ln == LN_PUPPETPALETTE) {
    ARITY(1);
    const char *palette = lv_cstr(r, a[0]);
    if (!eq(palette, "System - Win") && !eq(palette, "System - Mac"))
      error(d, "unsupported puppet palette", palette);
    // Bitmap colors are resolved offline; the stage has no mutable CLUT.
    return true;
  }
  if (ln == LN_PUPPETTRANSITION) {
    ARITY(1);
    int type = N(0);
    if (type != 23 && !(DIRECTOR64_EXTENDED_D6 && (type == 9 || type == 10))) {
      error(d, "unsupported transition", name);
      return true;
    }
    d->transition_type = (unsigned)type;
    d->transition_serial++;
    d->transition_duration = 15;
    d->transition_chunk = 1;
    return true;
  }
#if DG_D10
  // An unrecognized method on a sprite dispatches to its behaviors — the
  // dot form of sendSprite, which authored show/hide toolkits use.
  if (argc >= 1 && lv_type(a[0]) == LV_SPRITE && argc + 1 <= LV_LOCALS - 4) {
    lv_t forwarded[LV_LOCALS];
    forwarded[0] = a[0];
    forwarded[1] = lv_text(r, name, false);
    if (argc > 1) memcpy(forwarded + 2, a + 1, (argc - 1) * sizeof(lv_t));
    return call(r, "sendsprite", argc + 1, forwarded, out, yield);
  }
#endif
  return false;
#undef N
#undef ARITY
}
static void trace(lv_runtime_t *r, lv_t value) {
  dg_runtime_t *d = r->context;
  char text[512];
  if (d->platform.trace && lv_format(r, value, text, sizeof(text)))
    d->platform.trace(d->context, text);
}
void dg_init(dg_runtime_t *d, lv_runtime_t *r, dg_platform_t platform,
             void *context, const char *const *names, unsigned count,
             uint32_t seed) {
  memset(d, 0, sizeof(*d));
  d->values = r;
  d->platform = platform;
  d->context = context;
  d->mouse_x = 320;
  d->mouse_y = 240;
  d->sound_level = 7;
#if DG_MODERN
  d->color_depth = 32;
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++)
    d->channel_volume[ch] = 255;
#else
  d->color_depth = 8;
#endif
  d->float_precision = 4;
  lv_init(r,
          (lv_services_t){
              .get = get,
              .set = set,
              .reference = reference,
              .call = call,
              .trace = trace,
#if DG_MODERN
              .resolve = resolve_script,
              .script = script_identity,
              .format = format_reference,
#endif
          },
          d, names, count, seed);
  r->roots[0] = lv_list(r, 1, (lv_t[]){lv_text(r, "C:\\", false)}, false);
#if DG_D5
  r->roots[DG_BEHAVIOR_ROOT+DG_SPRITES] = lv_list(r, 0, NULL, false);
#endif
}
static void reset_score(dg_runtime_t *d) {
  memset(d->score, 0, sizeof(d->score));
  // Specs store normalized opacity, not D6's raw transparency byte. An
  // untouched score channel is raw zero, hence fully opaque when activated.
  // Rewinds must preserve this default even before its first sprite delta.
  for (unsigned ch = 6; ch < sizeof(d->score) / sizeof(d->score[0]); ch++)
    d->score[ch].blend = 100;
}
static void apply(dg_sprite_t *s, const dg_spec_t *v, unsigned mask) {
  if (mask & DG_MEMBER)
    set_member(s, v->member);
  if (mask & DG_POSITION) {
    s->value.x = v->x;
    s->value.y = v->y;
  }
  if (mask & (DG_WIDTH | DG_MEMBER))
    s->value.width = v->width;
  if (mask & (DG_HEIGHT | DG_MEMBER))
    s->value.height = v->height;
  if (mask & DG_INK) {
    s->value.ink = v->ink;
    s->stretch = v->stretch;
    s->trails = v->trails;
  }
#if DG_MODERN
  if (mask & DG_FORE) {
    s->value.fore_rgb = v->fore_rgb;
    s->value.flags = (s->value.flags & ~16) | (v->flags & 16);
  }
  if (mask & DG_BACK) {
    s->value.back_rgb = v->back_rgb;
    s->value.flags = (s->value.flags & ~32) | (v->flags & 32);
  }
  if (mask & DG_ROTATION)
    s->value.rotation = v->rotation;
  if (mask & DG_SKEW)
    s->value.skew = v->skew;
#endif
  if (mask & DG_FORE)
    s->value.fore = v->fore;
  if (mask & DG_BACK)
    s->value.back = v->back;
  if (mask & DG_THICKNESS)
    s->value.thickness = v->thickness;
  if (mask & DG_BLEND)
    s->value.blend = v->blend;
  if (mask & DG_TYPE)
    s->value.type = v->type;
  if (mask & DG_MOVEABLE) {
    s->value.flags = v->flags;
    s->moveable = !!(v->flags & 128);
  }
}
static void schedule_frame(dg_runtime_t *d, uint64_t base, bool reset) {
  unsigned tempo = d->puppet_tempo ? d->puppet_tempo : d->tempo;
  if (!tempo)
    tempo = 30;
  if (reset || tempo != d->clock_tempo)
    d->frame_remainder = 0;
  d->clock_tempo = tempo;
  uint64_t period = 60000000u / tempo;
  d->frame_remainder += 60000000u % tempo;
  if (d->frame_remainder >= tempo) {
    period++;
    d->frame_remainder -= tempo;
  }
#if DG_MODERN
  if (d->score_delay && !d->puppet_tempo)
    period = (uint64_t)d->score_delay * 60000000;
#endif
#if DG_D5
  unsigned raw=d->score[1].type;
  d->score_wait=raw>=128 && raw<196?raw:0;
#endif
#if DG_D5 || DG_D10
  d->actor_cursor=0;
#endif
  d->frame_deadline = base + period;
  d->frame_stalled = false;
}
#if DG_EXTENDED
static bool d6_queue_cleanup(dg_runtime_t *, unsigned, bool, bool, bool);
// Instantiate a channel's authored behaviors into its root slot, run each
// constructor and apply the score's parameter proplist. Channel zero is the
// script channel, whose behaviors carry the frame's authored properties —
// which narration cue to speak, how long to hold for it — so they need a real
// instance to hold them just as a sprite behavior does.
static void attach_behaviors(dg_runtime_t *d, unsigned channel,
                             const dg_behavior_t *behaviors, unsigned count) {
  lv_runtime_t *r = d->values;
  lv_t list = lv_list(r, 0, NULL, false);
  r->roots[DG_BEHAVIOR_ROOT + channel] = list;
  unsigned attached = 0;
  for (unsigned n = 0; n < count && !r->failed; n++) {
    const dg_behavior_t *behavior = &behaviors[n];
#if DG_D10
    // The authored score can reference a deleted script member; the
    // original attaches nothing there. A dangling instance must never
    // resolve by name into another movie's handlers.
    if (!dg_member(d, behavior->script)) {
      if (d->platform.trace) {
        char text[64];
        snprintf(text, sizeof(text), "BEHAVIOR_MEMBER_ABSENT %lu",
                 (unsigned long)behavior->script);
        d->platform.trace(d->context, text);
      }
      continue;
    }
#endif
    lv_t instance = lv_instance(r, lv_make(LV_SCRIPT, (int32_t)behavior->script));
    lv_set_at(r, list, ++attached, instance);
    const lv_movie_t *owner = NULL;
    lv_t script = lv_make(LV_SCRIPT, (int32_t)behavior->script);
    // Dispatch keeps collection running: a thousand-channel score's
    // constructors would otherwise fill the heap inside one atomic span.
    if (resolve_script(r, script, "new", &owner))
      lv_dispatch_method(r, r->depth ? &r->frames[r->depth - 1] : NULL,
                         "new", instance, 0, NULL);
    lv_set(r, NULL, "spritenum", instance, lv_num(channel));
    if (*behavior->parameters) {
      lv_t parameters = lv_literal(r, behavior->parameters);
      r->roots[DG_SCRATCH_ROOT] = parameters;
      if (lv_type(parameters) != LV_PROPLIST) {
        error(d, "invalid behavior parameters", behavior->parameters);
        r->roots[DG_SCRATCH_ROOT] = (lv_t){0};
        return;
      }
      for (unsigned p = 1; p <= lv_count(r, parameters); p++) {
        const char *key = lv_cstr(r, lv_at(r, parameters, p * 2 - 1));
        lv_set(r, NULL, key, instance, lv_at(r, parameters, p * 2));
      }
      r->roots[DG_SCRATCH_ROOT] = (lv_t){0};
    }
  }
}
#endif
static bool seek_now(dg_runtime_t *d, unsigned frame) {
  // Director clamps a go() past either end of the score; it does not fail.
  // Willy's credits movie runs go(the frame + 1) on every click including the
  // one on its last frame, and an authored go(the frame - 1) reaches zero the
  // same way from frame one. Traced, because an internal seek out of range
  // would be a runtime bug rather than an authored edge.
  unsigned last = d->movie->frame_count ? d->movie->frame_count : 1;
  if (!frame || frame > last) {
    if (d->platform.trace) {
      char report[80];
      snprintf(report, sizeof(report), "FRAME_CLAMPED %u of %u %.20s", frame,
               last, d->movie->code->name);
      d->platform.trace(d->context, report);
    }
    frame = frame ? last : 1;
  }
  bool rewind = frame <= d->frame;
  if (frame == d->frame) {
#if DG_MODERN
    if (d->score[1].tempo || d->score[1].delay)
      d->puppet_tempo = 0;
#else
    if (d->score[1].type)
      d->puppet_tempo = 0;
#endif
    schedule_frame(d, d->clock, true);
    return true;
  }
  unsigned first = rewind ? 1 : d->frame + 1;
  bool tempo_command = false;
  unsigned old_tempo = d->tempo;
  if (rewind) {
    reset_score(d);
#if DG_MODERN
    d->score_delay = 0;
#endif
    d->tempo = d->movie->tempo ? d->movie->tempo : 30;
  }
  // Resident, not a 3 KB stack array: the frame advance calls into the
  // interpreter (attachments, instances, collection) and a wide frame here
  // moved all of that down the C stack every frame.
  // Zero on entry: the pass clears the entries it set before returning.
  uint16_t *changed = d->seek_changed;
  for (unsigned f = first; f <= frame && f <= d->movie->frame_count; f++) {
    const dg_frame_t *source = &d->movie->frames[f - 1];
    for (unsigned n = 0; n < source->count; n++) {
      const dg_delta_t *delta = &d->movie->deltas[source->first + n];
      unsigned ch = delta->channel;
      if (ch >= DG_SCORE_CHANNELS) {
        error(d, "score channel out of range", d->movie->code->name);
        return false;
      }
      d->score[ch] = delta->value;
#if DG_MODERN
      unsigned command_tempo = delta->value.tempo;
      if (ch == 1) {
        // An explicit zero command ends a wait while retaining the cached FPS.
        // INTRO writes FPS12, delay1s, then zero in its first three frames.
        d->score_delay = delta->value.delay;
        if (command_tempo || delta->value.delay)
          tempo_command = true;
      }
#else
      unsigned command_tempo = delta->value.type;
#endif
      if (ch == 1 && command_tempo) {
        if (command_tempo > DG_MAX_TEMPO) {
          error(d, "invalid normalized score tempo", d->movie->code->name);
          return false;
        }
        d->tempo = command_tempo;
        tempo_command = true;
      }
      if (ch >= 6)
        changed[ch - 5] |= delta->mask | DG_TOUCHED;
    }
  }
#if DG_D10
  // Entering a new frame-behavior span delivers beginSprite to the script
  // channel exactly as sprite spans do; the behavior's properties persist on
  // its instance across the span's frames.
#if DG_EXTENDED
  // Key the span on the parameter block, not the script member. The castle
  // introduction's twelve narration cues are all one FrameSpeech behavior
  // with a different p_nSpeech each; keying on the member would attach the
  // first cue and then sit silent through the other eleven.
  if (d->score[0].behaviors != d->frame_behaviors ||
      d->score[0].script != d->frame_script_member) {
    d->frame_behaviors = d->score[0].behaviors;
    d->frame_script_member = d->score[0].script;
    attach_behaviors(d, 0, d->score[0].behaviors, d->score[0].behavior_count);
    d->frame_script_begin_pending =
        d->frame_script_member != 0 || d->score[0].behavior_count != 0;
  }
#else
  if (d->score[0].script != d->frame_script_member) {
    d->frame_script_member = d->score[0].script;
    d->frame_script_begin_pending = d->frame_script_member != 0;
  }
#endif
#endif
#if DG_MODERN
  // What event_channel_changed did for every channel, done once: the quiet
  // bits of every channel clear at each frame, and the mouse hit is stale.
  memset(d->event_quiet, 0, sizeof(d->event_quiet));
#if DG_D7_OR_D10
  d->mouse_hit_valid = false;
#endif
#endif
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    // A channel no delta wrote holds the sprite and the score record it held
    // after the last seek; only a rewind, which rewrote the score, or a delta
    // can make them differ. Reading the mask alone keeps the pass off the
    // hundreds of sprite and score lines the frame does not touch.
    if (!rewind && !changed[i]) continue;
    dg_sprite_t *s = &d->sprites[i];
    const dg_spec_t *v = &d->score[i + 5];
#if DG_MODERN
#if DG_EXTENDED
    // A span that begins here gets its own instances and its own beginSprite,
    // even where it re-uses the behavior block the last one had: three
    // one-frame sprites carrying the same behavior are three sprites, and the
    // score says so by writing the whole channel record each time.
    if (s->value.behaviors != v->behaviors ||
        ((changed[i] & DG_SPAN) && v->behavior_count)) {
      attach_behaviors(d, i, v->behaviors, v->behavior_count);
      s->value.behaviors = v->behaviors;
      s->value.behavior_count = v->behavior_count;
      d->begin_pending[i] = v->behavior_count > 0;
      if (d->begin_pending[i] && d->begin_cursor > i) d->begin_cursor = i;
    }
#else
    if (s->value.script != v->script) {
      lv_t list = lv_list(d->values, 0, NULL, false);
      d->values->roots[DG_BEHAVIOR_ROOT + i] = list;
      if (v->script) {
        lv_t instance = lv_instance(
            d->values, lv_make(LV_SCRIPT, (int32_t)v->script));
        d->values->roots[DG_BEHAVIOR_ROOT + i] = instance;
        lv_set(d->values, NULL, "spritenum", instance, lv_num(i));
        list = lv_list(d->values, 1, &instance, false);
        d->values->roots[DG_BEHAVIOR_ROOT + i] = list;
        d->begin_pending[i] = true;
        if (d->begin_cursor > i)
          d->begin_cursor = i;
      }
    }
#endif
#endif
    s->value.script = v->script;
    unsigned mask = rewind ? DG_ALL : (changed[i] & DG_ALL);
    // A channel the frame leaves alone has nothing to release or apply;
    // deciding that from the mask alone keeps the pass off the sprite's
    // flag line for the hundreds of channels a frame does not touch.
    if (mask) {
      // D6 releases automatic control when the score writes that property,
      // including dimensions when it writes the cast. Whole-sprite puppets
      // stay.
      unsigned release = mask | ((mask & DG_MEMBER) ? DG_SIZE : 0);
      if (!s->puppet)
        s->auto_mask &= ~release;
      mask &= ~s->auto_mask;
      if (!s->puppet && mask) {
        apply(s, v, mask);
#if DG_D5
        d5_media_init(d,s);
#endif
        dg_sprite_changed(d, i);
      }
    }
#if DG_MODERN
    // A score can replace a behavior while puppet control suppresses visual
    // deltas, so the behavior-root change also refreshes event eligibility.
    event_channel_changed(d, i);
#endif
  }
  // Clear the masks the deltas set (a walk over the same few records) rather
  // than the whole table every frame.
  for (unsigned f = first; f <= frame && f <= d->movie->frame_count; f++) {
    const dg_frame_t *source = &d->movie->frames[f - 1];
    for (unsigned n = 0; n < source->count; n++) {
      unsigned ch = d->movie->deltas[source->first + n].channel;
      if (ch >= 6) changed[ch - 5] = 0;
    }
  }
  d->frame = frame;
  d->frame_tick = d->ticks;
  d->frame_serial++;
  if (tempo_command || old_tempo != d->tempo)
    d->puppet_tempo = 0;
  schedule_frame(d, d->clock, true);
#if DG_D5
  d5_score_sounds(d);
#else
  for (unsigned ch = 0; ch < 2; ch++) {
    if (d->sound_puppet[ch])
      continue;
    uint32_t id = d->score[ch ? 3 : 4].member;
    if (id != d->sounds[ch]) {
      const dg_member_t *m = dg_member(d, id);
      if (m && m->type != 6) {
        error(d, "score sound missing", d->movie->code->name);
        return false;
      }
      if (id && !m && d->platform.trace)
        d->platform.trace(d->context, "SCORE_SOUND_EMPTY");
      d->sounds[ch] = id;
      d->sound_serial[ch]++;
      play_member(d, ch, m);
    }
  }
#endif
  return !d->values->failed;
}
bool dg_seek(dg_runtime_t *d, unsigned frame) {
#if DG_EXTENDED
  if (d6_queue_cleanup(d, frame, false, false, false)) return !d->values->failed;
#endif
  return seek_now(d, frame);
}
static bool event_with_mask(dg_runtime_t *, const char *, unsigned, unsigned);
#if DG_MODERN
enum {
  EVENT_PREPARE = 1u << 0, EVENT_ENTER = 1u << 1, EVENT_EXIT = 1u << 2,
  EVENT_BEGIN = 1u << 3, EVENT_DOWN = 1u << 4, EVENT_UP = 1u << 5
};
static unsigned standard_event(const char *event) {
  static const char *const names[] = {"prepareframe", "enterframe", "exitframe",
                                     "beginsprite", "mousedown", "mouseup"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (eq(event, names[i]))
      return 1u << i;
  return 0;
}
// `quiet`, when given, is a bitmap of channels to pass over as though they
// held no member at all.
static unsigned next_event_channel(const dg_runtime_t *d, unsigned cursor,
                                   const uint32_t *quiet) {
  if (cursor >= DG_SPRITES)
    return DG_SPRITES;
  unsigned word = cursor >> 5;
  uint32_t bit = UINT32_C(1) << (cursor & 31);
  uint32_t active = d->event_channels[word] & ~(quiet ? quiet[word] : 0u);
  if (active & bit)
    return cursor;
  active &= ~(bit - 1);
  while (!active && ++word < (DG_SPRITES + 31) / 32)
    active = d->event_channels[word] & ~(quiet ? quiet[word] : 0u);
  unsigned next = DG_SPRITES;
  if (active) {
    // Portable five-step bit scan; VR4300 has no count-trailing-zero opcode.
    unsigned first = 0;
    if (!(active & UINT32_C(0xffff))) { active >>= 16; first += 16; }
    if (!(active & UINT32_C(0xff))) { active >>= 8; first += 8; }
    if (!(active & UINT32_C(0xf))) { active >>= 4; first += 4; }
    if (!(active & UINT32_C(3))) { active >>= 2; first += 2; }
    if (!(active & UINT32_C(1))) first++;
    next = word * 32 + first;
  }
  return next;
}
static void skip_empty_events(dg_runtime_t *d, unsigned *guard,
                              const uint32_t *quiet) {
  // Inspect live values: a preceding handler can assign a later sprite's
  // member or behavior list during the same phase. Non-VOID empty lists still
  // take the established sprite_event path (including currentSpriteNum).
  // Charge every skipped channel against the original dispatch bound, so this
  // optimization cannot change how much source code runs in a service tick.
  // The outer loop also drains one pending beginSprite at a time. Let it keep
  // doing so until that queue is exhausted, even when a begin handler is absent.
  if (d->begin_cursor < DG_SPRITES)
    return;
  unsigned skipped =
      next_event_channel(d, d->event_cursor, quiet) - d->event_cursor;
  if (skipped > 4096 - *guard)
    skipped = 4096 - *guard;
  d->event_cursor += skipped;
  *guard += skipped;
}
// Which event_quiet row a standard frame event's bit selects. Only the three
// frame events are indexed; every caller passes one of them, and the write
// side tests the mask before recording so a mouse event can never land here.
static unsigned quiet_row(unsigned mask) {
  return mask == EVENT_PREPARE ? 0u : mask == EVENT_ENTER ? 1u : 2u;
}
static bool sprite_event(dg_runtime_t *d, const char *event, unsigned channel,
                          unsigned mask) {
  if (!channel || channel >= DG_SPRITES)
    return false;
  lv_t list = d->values->roots[DG_BEHAVIOR_ROOT + channel];
  if (lv_type(list) == LV_VOID) {
    unsigned member = d->sprites[channel].value.member;
    bool started = member && event_with_mask(d, event, member, mask);
    if (!started && (mask & (EVENT_PREPARE | EVENT_ENTER | EVENT_EXIT)))
      d->event_quiet[quiet_row(mask)][channel >> 5] |=
          UINT32_C(1) << (channel & 31);
    return started;
  }
  d->current_event_sprite = channel;
#ifdef DIRECTOR64_DEBUG_EVENTS
  if (!strcmp(event, "mouseup") || !strcmp(event, "mousedown"))
    fprintf(stderr, "DBG sprite_event %s ch=%u count=%u\n", event, channel,
            lv_count(d->values, list));
#endif
  // The recovered score binds at most one behavior per sprite. Runtime lists
  // may also be assigned by Lingo, so resolve each receiver without storing
  // an overlay address in persistent state.
  for (unsigned i = 1; i <= lv_count(d->values, list); i++) {
    lv_t instance = lv_at(d->values, list, i);
    if (lv_start_method(d->values, instance, event, 0, NULL)) {
#ifdef DIRECTOR64_DEBUG_EVENTS
      if (!strcmp(event, "mouseup") || !strcmp(event, "mousedown"))
        fprintf(stderr, "DBG started %s ch=%u i=%u\n", event, channel, i);
#endif
#if DG_EXTENDED
      d->behavior_channel = channel;
      d->behavior_cursor = i + 1;
      snprintf(d->behavior_event, sizeof(d->behavior_event), "%s", event);
#endif
      return true;
    }
  }
  // A channel whose behaviors and member all decline the event is silent for
  // it until one of them changes, exactly as a channel with no behaviors at
  // all is. Willy's workshop carries a behavior on most of its sprites and
  // almost none of them handle a frame event, so this was a hundred method
  // resolutions a tick that could only fail. event_channel_changed clears the
  // bit on a member, behavior-list or scriptInstanceList change, and nothing
  // in the corpus mutates that list in place.
  bool started = d->sprites[channel].value.member &&
                 event_with_mask(d, event, d->sprites[channel].value.member, mask);
  if (!started && (mask & (EVENT_PREPARE | EVENT_ENTER | EVENT_EXIT)))
    d->event_quiet[quiet_row(mask)][channel >> 5] |=
        UINT32_C(1) << (channel & 31);
  return started;
}
// Dispatch one phase's channels until one of them starts a handler. A
// sprite_event that starts nothing leaves the runtime exactly as it found
// it, so going back around the service loop between channels only re-runs
// its checks — and a station frame visits two hundred and forty channels
// across its three phases, almost none of which own a handler. The guard is
// still charged per channel, so a tick dispatches exactly as much source
// code as it did before. Returns true when the caller must return to the
// service loop: a handler is running, or a script error needs recovering.
static bool dispatch_phase(dg_runtime_t *d, const char *event, unsigned mask,
                           unsigned *guard) {
  const uint32_t *quiet = d->event_quiet[quiet_row(mask)];
  for (;;) {
    skip_empty_events(d, guard, quiet);
    if (*guard >= 4096 || d->event_cursor >= DG_SPRITES)
      return false;
    unsigned channel = d->event_cursor++;
    if (sprite_event(d, event, channel, mask) || d->values->failed)
      return true;
    if (++*guard >= 4096)
      return false;
    // While beginSprites are still queued the outer loop drains one between
    // channels, and their slots precede the next pending begin and a later
    // prepare. Batching past that would reorder the two, so this only runs
    // ahead once the queue is empty — which is every frame of ordinary play.
    if (d->begin_cursor < DG_SPRITES)
      return false;
  }
}
// The script channel's frame events. Its behaviors receive them on their
// instances, where the score's authored properties live, and the frame script
// member is the fallback — the same order sprite_event uses for a sprite and
// its member.
static bool frame_event(dg_runtime_t *d, const char *event, unsigned member,
                        unsigned mask) {
#if DG_EXTENDED
  lv_runtime_t *r = d->values;
  lv_t list = r->roots[DG_BEHAVIOR_ROOT];
  if (lv_type(list) != LV_VOID) {
    d->current_event_sprite = 0;
    for (unsigned i = 1; i <= lv_count(r, list); i++) {
      if (!lv_start_method(r, lv_at(r, list, i), event, 0, NULL))
        continue;
      // Resume at the next behavior once this one finishes. The frame channel
      // is channel zero, so the cursor rather than the channel says whether a
      // list is mid-dispatch.
      d->behavior_channel = 0;
      d->behavior_cursor = (uint16_t)(i + 1);
      snprintf(d->behavior_event, sizeof(d->behavior_event), "%s", event);
      return true;
    }
  }
#endif
  return event_with_mask(d, event, member, mask);
}
#if DG_EXTENDED
// Deliver at most one due cue point, at the scope chain cuePassed uses: the
// frame's behaviors, then the frame script, then the loaded movie scripts.
// Returns true when a handler started and the service loop must run it.
static bool cue_dispatch(dg_runtime_t *d) {
  if (!d->platform.sound_position)
    return false;
  lv_runtime_t *r = d->values;
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) {
    if (d->cue_state[ch].serial != d->sound_serial[ch]) {
      d->cue_state[ch].serial = d->sound_serial[ch];
      d->cue_state[ch].member = dg_member(d, d->sounds[ch]);
      d->cue_state[ch].next = 0;
    }
    const dg_member_t *m = d->cue_state[ch].member;
    if (!m || d->cue_state[ch].next >= m->cue_count)
      continue;
    const dg_cue_t *cue = &m->cues[d->cue_state[ch].next];
    if (d->cue_state[ch].polled != d->ticks) {
      d->cue_state[ch].polled = d->ticks;
      d->cue_state[ch].position = d->platform.sound_position(d->context, ch);
    }
    if (d->cue_state[ch].position < cue->milliseconds)
      continue;
    // Charge the cue as delivered before dispatching: a handler that stops
    // the channel or plays another sound reruns this from the new serial,
    // and one that does neither must not see the same cue again.
    d->cue_state[ch].next++;
    // Director names the channel so a generic handler can recover it from
    // the symbol's text; Willy's read the last character of exactly that.
    char channel_name[16];
    snprintf(channel_name, sizeof(channel_name), "sound%u", ch + 1);
    lv_t args[3] = {lv_text(r, channel_name, true), lv_num(d->cue_state[ch].next),
                    lv_text(r, cue->name, false)};
    // The name outlives several dispatch attempts, each of which may collect.
    r->roots[DG_SCRATCH_ROOT] = args[2];
    bool started = false;
    lv_t list = r->roots[DG_BEHAVIOR_ROOT];
    if (lv_type(list) != LV_VOID)
      for (unsigned i = 1; i <= lv_count(r, list) && !started; i++)
        started = lv_start_method(r, lv_at(r, list, i), "cuepassed", 3, args);
    unsigned member = d->score[0].script;
    const dg_movie_t *frame_movie = started ? NULL : dg_loaded(d, member >> 20);
    unsigned cast = (member >> 16) & 15;
    if (frame_movie && cast && cast <= frame_movie->cast_count)
      started = lv_start_cast_args(r, frame_movie->code, "cuepassed",
                                   member & 65535,
                                   frame_movie->casts[cast - 1].name, 3, args);
    for (unsigned i = 0; i < d->loaded_count && !started; i++)
      started = lv_start(r, d->loaded[i]->code, "cuepassed", 0, 3, args);
    r->roots[DG_SCRATCH_ROOT] = (lv_t){0};
    if (started || r->failed)
      return true;
  }
  return false;
}
#endif
#endif
static bool event_with_mask(dg_runtime_t *d, const char *event, unsigned member,
                             unsigned bit) {
  if (!member) {
    if (lv_start(d->values, d->movie->code, event, 0, 0, NULL))
      return true;
#if DG_MODERN
    for (unsigned i = 1; i < d->loaded_count; i++) {
#if DG_D10
      // Controller-window code and its cast archives stay resident for
      // explicit calls, never for another movie's event broadcast.
      if (d10_window_only(d, d->loaded[i]))
        continue;
#endif
      if (lv_start(d->values, d->loaded[i]->code, event, 0, 0, NULL))
        return true;
    }
#endif
    return false;
  }
#if DG_MODERN
  unsigned slot = (member * UINT32_C(0x9e3779b1)) >>
#if DG_CAP_WIDE
      23;
#else
      25;
#endif
  if (bit && d->event_misses[slot].member == member &&
      (d->event_misses[slot].missing & bit))
    return false;
#else
  (void)bit;
#endif
  const dg_movie_t *movie = dg_loaded(d, member >> 20);
  if (!movie)
    return false;
  unsigned cast = (member >> 16) & 15;
  if (!cast || cast > movie->cast_count)
    return false;
#if DG_MODERN
  bool started = lv_start_cast(d->values, movie->code, event, member & 65535,
                                movie->casts[cast - 1].name);
  if (bit && !started) {
    if (d->event_misses[slot].member != member) {
      d->event_misses[slot].member = member;
      d->event_misses[slot].missing = 0;
    }
    d->event_misses[slot].missing |= bit;
  }
  return started;
#else
  return lv_start_cast(d->values, movie->code, event, member & 65535,
                       movie->casts[cast - 1].name);
#endif
}
bool dg_event(dg_runtime_t *d, const char *event, unsigned member) {
#if DG_MODERN
  return event_with_mask(d, event, member, standard_event(event));
#else
  return event_with_mask(d, event, member, 0);
#endif
}
#if DG_EXTENDED
// Continue input events after native handlers yield, preserving their source
// ordering. Hover runs with the current button state before mouseUp activation.
static bool d6_input(dg_runtime_t *d) {
  if (d->input_stage == 10) {
    d->input_stage = 11;
    d->pass_event = false;
    if (sprite_event(d, "keydown", d->text_sprite, 0)) return true;
    d->pass_event = true;
  }
  if (d->input_stage == 11) {
    d->input_stage = 12;
    if (d->pass_event && dg_event(d, "keydown", 0)) return true;
  }
  if (d->input_stage == 12) {
    d->input_stage = 13;
    if (event_with_mask(d, "idle", d->score[0].script, 0)) return true;
  }
  if (d->input_stage == 13) {
    d->input_stage = 0;
    if (!d->text_replacement[d->text_index]) d->text_pending = false;
  }
  if (!d->input_stage && d->text_pending) {
    const dg_member_t *m=dg_member(d,d->sprites[d->text_sprite].value.member);
    char text[21];snprintf(text,sizeof(text),"%s",d6_text(d,m));
    unsigned length=(unsigned)strlen(text);
    d->text_key=(unsigned char)d->text_replacement[d->text_index];
    if (d->text_key) {
      if (length<20) {text[length++]=(char)d->text_key;text[length]=0;}
      d->text_index++;
      d->text_key_code=0;
    } else {d->text_key=8;d->text_key_code=51;}
    set(d->values,"text",lv_make(LV_MEMBER, (int32_t)m->id),lv_text(d->values,text,false));
    d->sel_start=d->sel_end=length;
    d->input_stage = 10;
    if (*d->key_down_script && dg_event(d, d->key_down_script, 0)) return true;
    return true;
  }
  if (d->input_stage == 1) {
    d->input_stage = 0;
    if (sprite_event(d, "mouseenter", d->input_channel, 0)) return true;
  }
  // Hover transitions sample once per tick, like Director's per-frame
  // rollover. Every dispatched handler invalidates the cached hit, and
  // re-scanning the score between each queued event starves the 60 Hz
  // service budget on console-scale stages.
  if (d->hover_tick == d->ticks) return false;
  d->hover_tick = d->ticks;
  unsigned hit = dg_mouse_hit(d, d->mouse_x, d->mouse_y);
  if (!d->input_stage && hit != d->hover_sprite) {
    unsigned old = d->hover_sprite;
    d->hover_sprite = d->input_channel = hit;
    d->input_stage = 1;
    if (sprite_event(d, "mouseleave", old, 0)) return true;
    return true;
  }
  return false;
}
static bool d6_idle(dg_runtime_t *d) {
  if (d->input_stage == 2) {
    d->input_stage = 3;
    d->pass_event = false;
    if (!event_with_mask(d, "idle", d->score[0].script, 0)) d->pass_event = true;
    return true;
  }
  if (d->input_stage == 3) {
    d->input_stage = 0;
    return d->pass_event && dg_event(d, "idle", 0);
  }
  if (!d->input_stage && d->idle_tick != d->ticks) {
    d->idle_tick = d->ticks;
    d->input_stage = 2;
    if (sprite_event(d, "mousewithin", d->hover_sprite, 0)) return true;
    return true;
  }
  return false;
}
#endif
void dg_stop_sounds(dg_runtime_t *d) {
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) {
#if DG_MODERN
    cancel_fade(d, ch);
#endif
    if (d->sounds[ch] || d->sound_puppet[ch]) {
      if (d->platform.sound)
        d->platform.sound(d->context, ch, NULL);
      d->sound_serial[ch]++;
    }
    d->sounds[ch] = 0;
    d->sound_puppet[ch] = false;
  }
}
bool dg_enter(dg_runtime_t *d, const dg_movie_t *movie, unsigned frame,
              const dg_movie_t *const *shared, unsigned count) {
  if (d->values->depth || count + 1 > DG_FILES || count > LV_SHARED || !movie) {
    error(d, "invalid scene transition", "state");
    return false;
  }
#if DG_D5
  if(d->window_open) {
    dg_runtime_t *stage=malloc(sizeof(*stage));
    if(!stage){error(d,"movie window memory","allocation");return false;}
    *stage=*d;stage->window_open=false;stage->next_movie[0]=0;
    stage->native_dialog=false;
    d->values->roots[LV_ROOTS-1]=lv_list(d->values,LV_ROOTS,d->values->roots,false);
    if(d->values->failed){free(stage);return false;}
    d->suspended_stage=stage;d->window_open=false;d->native_dialog=true;
    d->source_paused=false;
    d->values->roots[DG_BEHAVIOR_ROOT+DG_SPRITES]=lv_list(d->values,0,NULL,false);
  }
#endif
  dg_stop_sounds(d);
  d->movie = movie;
  d->cursor = (dg_cursor_t){0}; // A new Score starts with its default arrow.
#if DG_MODERN
  if(*d->next_movie_label) {
    frame=frame_number(d,lv_text(d->values,d->next_movie_label,false));
    d->next_movie_label[0]=0;
    // An unknown label is a recovered script alert; play the movie from 1.
    if(d->values->failed)recover_script_error(d);
    if(!frame)frame=1;
  }
#endif
  d->values->current = movie->code;
#if DG_EXTENDED
  d->behavior_channel = d->behavior_cursor = 0;
  d->cleanup_active = d->ending_sprites = d->movie_ready = d->quit_requested = false;
  // Cue state holds a member out of the score being left behind.
  memset(d->cue_state, 0, sizeof(d->cue_state));
#endif
  d->loaded[0] = movie;
  d->loaded_count = 1;
  d->values->shared_count = 0;
  // Cached handler addresses belong to the just-unloaded native overlays.
  memset(d->values->call_cache, 0, sizeof(d->values->call_cache));
  d->values->call_generation++;
  d->values->name_id_for = NULL; // pooled names may now belong to another overlay
  d->values->symbol_for = NULL;
  memset(d->mouse_memo, 0, sizeof(d->mouse_memo));
#if DG_MODERN
  memset(d->event_misses, 0, sizeof(d->event_misses));
  memset(d->event_channels, 0, sizeof(d->event_channels));
  memset(d->event_quiet, 0, sizeof(d->event_quiet));
  memset(d->film_member, 0, sizeof(d->film_member));
  memset(d->film_loop_channel, 0, sizeof(d->film_loop_channel));
  memset(d->film_channels, 0, sizeof(d->film_channels));
  memset(d->film_loop_member, 0, sizeof(d->film_loop_member));
#endif
  memset(d->seek_changed, 0, sizeof(d->seek_changed));
  for (unsigned i = 0; i < count; i++) {
    d->loaded[d->loaded_count++] = shared[i];
    d->values->shared[d->values->shared_count++] = shared[i]->code;
  }
#if DG_D10
  // The controller window's code survives every stage movie change.
  d10_window_share(d);
  // Flash object bags belong to the departing movie's sprites.
  for (unsigned i = 0; i < d->flash_object_count; i++)
    d->values->roots[DG_FLASH_ROOT + i] = (lv_t){0};
  d->flash_object_count = 0;
#endif
#if DG_EXTENDED
  d6_restore_casts(d);
#endif
  reset_score(d);
  memset(d->sprites, 0, sizeof(d->sprites));
  memset(d->staged, 0, sizeof(d->staged));
  memset(d->stage_dirty, 0, sizeof(d->stage_dirty));
  d->stage_count = 0;
  d->trail_count = 0;
  d->order_serial++;
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    d->sprites[i].visible = true;
#if DG_MODERN
    d->sprites[i].value.loc_z = i;
    d->begin_pending[i] = false;
    d->values->roots[DG_BEHAVIOR_ROOT + i] = (lv_t){0};
#endif
    d->sprites[i].value.blend = 100;
    dg_sprite_changed(d, i);
  }
#if DG_MODERN
  d->stage_color = movie->stage_color;
  d->score_delay = 0;
  d->begin_cursor = 1;
  d->hover_sprite = 0;
#endif
#if DG_D10
  // The script channel starts each movie unattached, so its first span
  // reattaches even where the new movie reuses the old one's frame script.
  d->frame_script_member = 0;
  d->frame_script_begin_pending = false;
#if DG_EXTENDED
  d->frame_behaviors = NULL;
  d->values->roots[DG_BEHAVIOR_ROOT] = (lv_t){0};
#endif
#endif
  d->frame = 0;
  d->next_frame = 0;
  d->phase = 0;
  d->event_cursor = 0;
  d->puppet_tempo = 0;
  d->tempo = movie->tempo ? movie->tempo : 30;
  d->next_movie[0] = 0;
  d->mouse_up_script[0] = 0;
#if DG_EXTENDED
  d->mouse_down_script[0] = d->key_down_script[0] = 0;
  d->mouse_down_dispatch = false;
  d->text_pending = false;
  d->input_stage = d->text_sprite = 0;
  d->sel_start = d->sel_end = 0;
  d->idle_tick = UINT32_MAX;
  memset(d->sprite_editable, 0, sizeof(d->sprite_editable));
#endif
#if DG_D5
  d->mouse_down_script[0] = 0;
  d->input_event = d->rollover_sprite = 0;
  d->stopped_movie = false;
#endif
  d->resume_tick = d->ticks;
#if !DG_EXTENDED
  d->field_count = 0;
#if DG_MODERN
  memset(d->field_fonts, 0, sizeof(d->field_fonts));
  memset(d->field_text_changed, 0, sizeof(d->field_text_changed));
#endif
#endif
  d->click_on = 0;
  d->drag_sprite = 0;
  // A controller press activating a route must not also activate the
  // destination under the same pointer while its button is still held.
  d->await_release = d->mouse_down;
  d->mouse_down = false;
  d->mouse_pressed = d->mouse_released = d->mouse_up_dispatch = false;
#if !DG_EXTENDED
  for (unsigned i = DG_FIELD_TEXT_ROOT; i < DG_FIELD_COLOR_ROOT + DG_FIELDS;
       i++)
    d->values->roots[i] = (lv_t){0};
#endif
  if (!dg_seek(d, frame ? frame : 1))
    return false;
#if DG_MODERN
  d->start_movie_pending = true;
  dg_event(d, "preparemovie", 0);
#else
  dg_event(d, "startmovie", 0);
#endif
  return true;
}
#if DG_EXTENDED
bool dg_edit_text(dg_runtime_t *d, unsigned channel, const char *text) {
  if (!channel || channel >= DG_SPRITES || strlen(text) > 20) return false;
  const dg_member_t *m = dg_member(d, d->sprites[channel].value.member);
  if (!dg_text_editable(d, channel) || d->values->depth || d->text_pending) return false;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
#if DG_D10
    // The on-screen keyboard types windows-1252 German letters into names
    // and spelling answers.
    if (*p == 0xC4 || *p == 0xD6 || *p == 0xDC || *p == 0xDF ||
        *p == 0xE4 || *p == 0xF6 || *p == 0xFC)
      continue;
#endif
    if (*p < 32 || *p > 126 || *p == '"') return false;
  }
  const char *before = lv_cstr(d->values, get(d->values, "text", lv_make(LV_MEMBER, (int32_t)m->id)));
  if (strcmp(before, text)) {
    // Replace the selection one character at a time through keyDown and idle,
    // so original length checks see exactly the same edits as desktop typing.
    snprintf(d->text_replacement,sizeof(d->text_replacement),"%s",text);
    d->text_index=0;
    d->text_sprite=channel;
    d->text_pending=true;
    d->sel_start=d->sel_end=0;
    set(d->values,"text",lv_make(LV_MEMBER, (int32_t)m->id),lv_text(d->values,"",false));
  }
  return !d->values->failed;
}
#endif
bool dg_tick(dg_runtime_t *d, int x, int y, bool down, unsigned budget) {
  // Any change to a sprite between ticks reports through dg_sprite_changed;
  // a tick boundary refreshes the draw order regardless.
  d->order_serial++;
#if DG_D7_OR_D10
  d->mouse_hit_valid = false;
#endif
#if DG_D5
  if(d->native_dialog){x-=112;y-=100;}
#endif
  d->ticks++;
#if DG_D5
  d5_media_tick(d);
#endif
#if DG_MODERN
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++)
    if (d->fade_duration[ch]) {
      uint32_t elapsed = d->ticks - d->fade_start[ch],
               duration = d->fade_duration[ch];
      d->channel_volume[ch] =
          elapsed >= duration
              ? 0
              : (uint64_t)d->fade_volume[ch] * (duration - elapsed) / duration;
    }
#endif
  d->clock += 1000000;
  d->mouse_x = x;
  d->mouse_y = y;
  if (d->await_release) {
    if (!down)
      d->await_release = false;
    down = false;
  }
#if DG_D5
  if(d->native_dialog && down && (x<0 || y<0 || x>=416 || y>=240)) {
    int global=lv_global_id(d->values,"gcontrol");
    if(!d->values->depth && global>=0)
      lv_start_method(d->values,d->values->globals[global],"miawzu",0,NULL);
    down=false;
  }
#endif
  bool pressed = down && !d->mouse_down;
#if DG_D5
  // Queue the release target before a yielding drag handler changes the stage.
  // Re-hitting after HAM's drag completes would send this release to the new
  // reward video and immediately stop it instead of completing the picture drag.
  if (!down && d->mouse_down)
    d->mouse_up_sprite = dg_mouse_hit(d, x, y);
#endif
  d->mouse_pressed |= pressed;
  d->mouse_released |= !down && d->mouse_down;
  d->mouse_down = down;
  if (pressed) {
#if DG_D5
    if(d->score_wait==128)d->score_wait=0;
#endif
    d->click_on = dg_mouse_hit(d, x, y);
    dg_sprite_t *s = &d->sprites[d->click_on];
    if (d->click_on && s->moveable) {
      d->drag_sprite = d->click_on;
      d->drag_offset_x = s->value.x - x;
      d->drag_offset_y = s->value.y - y;
    }
  }
  if (d->drag_sprite) {
    dg_sprite_t *s = &d->sprites[d->drag_sprite];
    set_position(d, s, x + d->drag_offset_x, y + d->drag_offset_y);
    dg_sprite_changed(d, d->drag_sprite);
    s->auto_mask |= DG_POSITION;
    if (!down)
      d->drag_sprite = 0;
  }
  return dg_service(d, budget);
}

bool dg_clock_advance(uint64_t *phase, uint64_t elapsed_us, unsigned *steps) {
  *steps = 0;
  if (elapsed_us > (UINT64_MAX - *phase) / 60)
    return false;
  *phase += elapsed_us * 60;
  uint64_t available = *phase / 1000000;
  *steps = available > 4 ? 4 : (unsigned)available;
  *phase -= (uint64_t)*steps * 1000000;
  return true;
}

static bool advance_frame(dg_runtime_t *d, unsigned next, bool timed) {
#if DG_EXTENDED
  if (d6_queue_cleanup(d, next, true, timed, false)) return !d->values->failed;
#endif
  // Retain sub-tick lateness, but do not burst through frames after a long
  // script wait. Director displays each score frame even when work runs late.
  uint64_t base = timed && !d->frame_stalled ? d->frame_deadline : d->clock;
  if (d->clock > base && d->clock - base > 1000000)
    base = d->clock;
  unsigned remainder = d->frame_remainder, tempo = d->clock_tempo;
#if DG_MODERN
  // Only the film-loop channels advance, and film_channel_changed keeps
  // that set and each channel's member current, so this reads nothing of
  // the other sprites on each authored go-to-the-current-frame loop.
  for (unsigned word = 0; word < (DG_SPRITES + 31) / 32; word++) {
    uint32_t bits = d->film_channels[word];
    for (unsigned n = 0; bits; n++, bits >>= 1) {
      if (!(bits & 1)) continue;
      unsigned i = word * 32 + n;
      dg_sprite_t *s = &d->sprites[i];
      if (!s->value.type)
        continue;
      const dg_member_t *m = d->film_loop_member[i];
      unsigned old = dg_film_pose(m, s);
      s->film_frame = m->film_loop ? (old + 1) % m->film_count
                                   : old + (old + 1 < m->film_count);
      if (s->film_frame != old)
        dg_sprite_changed(d, i);
    }
  }
#else
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    dg_sprite_t *s = &d->sprites[i];
    if (!s->value.type)
      continue;
    const dg_member_t *m = dg_member(d, s->value.member);
    if (!m || m->type != 2 || !m->film_count)
      continue;
    unsigned old = dg_film_pose(m, s);
    s->film_frame = m->film_loop ? (old + 1) % m->film_count
                                 : old + (old + 1 < m->film_count);
    if (s->film_frame != old)
      dg_sprite_changed(d, i);
  }
#endif
  // Advance existing loops first, so a score-assigned member starts at pose 0.
  if (!seek_now(d, next))
    return false;
  d->clock_tempo = tempo;
  d->frame_remainder = remainder;
  schedule_frame(d, base, !timed);
  d->phase = 0;
  d->event_cursor = 0;
  return true;
}

#if DG_EXTENDED
#include "d6_events.inc"
#endif
bool dg_transition_ready(const dg_runtime_t *d) {
  return d->next_movie[0] && !d->values->depth
#if DG_EXTENDED
         && d->movie_ready
#endif
         ;
}

bool dg_service(dg_runtime_t *d, unsigned budget) {
  lv_runtime_t *r = d->values;
  // Dispatch itself is bounded as well as the total native handler work.
  for (unsigned guard = 0; guard < 4096 && !d->quit; guard++) {
    // A script-class failure matches an original alert: discard the call
    // contexts and continue with the next queued event. Hard failures stop.
    if (r->failed && !recover_script_error(d))
      break;
    if ((int32_t)(d->ticks - d->resume_tick) < 0) {
      d->frame_stalled = true;
      break;
    }
    if (d->next_frame
#if DG_EXTENDED
        && !d->cleanup_active
#endif
    ) {
      unsigned next = d->next_frame;
      d->next_frame = 0;
      bool timed = d->phase >= 2 && d->clock >= d->frame_deadline;
      if (!advance_frame(d, next, timed)) {
        if (r->failed && r->script_error)
          continue;
        break;
      }
    }
#if DG_EXTENDED
    // D6 updateSprites runs beginSprite before startMovie and before a go
    // continuation observes the newly entered span (ScummVM startPlay).
    // seek_now rewinds begin_cursor whenever it queues a new attachment.
    // Once drained, avoid scanning every score slot between each event.
    if (d->begin_cursor < DG_SPRITES && !d->cleanup_active &&
        (!d->start_movie_pending || !r->depth)) {
      for (unsigned i = 1; i < DG_SPRITES && !r->failed; i++) {
        if (!d->begin_pending[i]) continue;
        d->begin_pending[i] = false;
        lv_t list = r->roots[DG_BEHAVIOR_ROOT + i];
        unsigned previous = d->current_event_sprite;
        d->current_event_sprite = i;
        for (unsigned n = 1; n <= lv_count(r, list) && !r->failed; n++) {
          lv_t instance = lv_at(r, list, n);
          lv_t script = lv_get(r, NULL, "script", instance);
          const lv_movie_t *owner = NULL;
          // Dispatch keeps collection running across the begin fan-out.
          if (resolve_script(r, script, "beginsprite", &owner))
            lv_dispatch_method(r, r->depth ? &r->frames[r->depth - 1] : NULL,
                               "beginsprite", instance, 0, NULL);
        }
        d->current_event_sprite = previous;
      }
    }
#endif
    if (r->depth) {
      if (!budget) {
        d->frame_stalled = true;
        break;
      }
      uint32_t before = r->steps;
#if DG_D7_OR_D10
      // A handler may change an ancestor or instance list without moving a
      // sprite, so cached mouse eligibility ends at every script execution.
      d->mouse_hit_valid = false;
#endif
      lv_run(r, budget);
      unsigned used = r->steps - before;
      budget = used >= budget ? 0 : budget - used;
      if (r->failed)
        continue; // recovered or stopped at the top of the loop
      // A go command changes the stage before its continuation runs. Other
      // yields still wait for their timer or the next platform service tick.
      if (d->next_frame || (d->next_movie[0] && r->depth && r->yielded
#if DG_EXTENDED
                           && !d->cleanup_active
#endif
                           ))
        continue;
      if (r->depth || r->yielded) {
        d->frame_stalled = true;
        break;
      }
    }
#if DG_EXTENDED
    if ((d->next_movie[0] || d->quit_requested) && !d->movie_ready) {
      if (d6_queue_cleanup(d, 0, false, false, true)) continue;
    }
#endif
    if (d->next_movie[0]) {
#if DG_D5
      if(!d->window_open && !d->stopped_movie) {
        d->stopped_movie=true;
        if(dg_event(d,"stopmovie",0))continue;
      }
#endif
      break;
    }
#if DG_EXTENDED
    // The cursor, not the channel, marks a list as mid-dispatch: the script
    // channel's behaviors live on channel zero.
    if (d->behavior_cursor) {
      lv_t list = r->roots[DG_BEHAVIOR_ROOT + d->behavior_channel];
      if (d->behavior_cursor <= lv_count(r, list)) {
        lv_t instance = lv_at(r, list, d->behavior_cursor++);
        d->current_event_sprite = d->behavior_channel;
        lv_start_method(r, instance, d->behavior_event, 0, NULL);
        continue;
      }
      d->behavior_channel = d->behavior_cursor = 0;
    }
    // A cue point belongs to the moment the sound reaches it, ahead of the
    // frame that would otherwise hold waiting for one.
    if (cue_dispatch(d)) continue;
#endif
#if DG_D5
    if(d->source_paused)break;
#endif
#if DG_MODERN
#if DG_D10
    if (d->frame_script_begin_pending) {
      d->frame_script_begin_pending = false;
      d->current_event_sprite = 0;
      if (frame_event(d, "beginsprite", d->frame_script_member, EVENT_BEGIN))
        continue;
    }
#endif
    if (d->start_movie_pending) {
      d->start_movie_pending = false;
      if (dg_event(d, "startmovie", 0))
        continue;
    }
    while (d->begin_cursor < DG_SPRITES && !d->begin_pending[d->begin_cursor])
      d->begin_cursor++;
    if (d->begin_cursor < DG_SPRITES) {
      unsigned i = d->begin_cursor++;
      d->begin_pending[i] = false;
      if (sprite_event(d, "beginsprite", i, EVENT_BEGIN))
        continue;
    }
#endif
    // Input remains responsive while a slow score waits for exitFrame.
#if DG_EXTENDED
    if (d6_input(d)) continue;
#endif
#if DG_CAP_KEYBOARD
    // The selected D8 media handles keyboard events at movie scope. Dispatch
    // at most one transition per service tick, leaving a frame opportunity
    // between a queued press and release (LO polls after its keyDown handler).
    if (d->key_count && d->key_event_tick != d->ticks) {
      unsigned index = d->key_first;
      bool down = d->key_events[index].down;
      if (down) {
        d->key_code = d->key_events[index].code;
        d->key_character = d->key_events[index].character;
      }
      d->key_first = (index + 1) % 32;
      d->key_count--;
      d->key_event_tick = d->ticks;
      if (dg_event(d, down ? "keydown" : "keyup", 0)) continue;
    }
#endif
#if DG_D5
    if(d5_input(d))continue;
#endif
#if DG_EXTENDED
    if (d->mouse_pressed) {
      d->mouse_pressed = false;
      d->mouse_down_dispatch = true;
      if (*d->mouse_down_script && dg_event(d, d->mouse_down_script, 0)) continue;
    }
    if (d->mouse_down_dispatch) {
      d->mouse_down_dispatch = false;
#else
    if (d->mouse_pressed) {
      d->mouse_pressed = false;
#endif
#if DG_D5
      d->input_event=1;d->input_stage=0;d->input_channel=d->click_on;d->pass_event=true;
      continue;
#elif DG_MODERN
      // Through frame_event, so the script channel's behaviors receive it on
      // their instances. Dispatching the frame script as a bare cast script
      // leaves `me` unbound, and a behavior reading its own properties then
      // sees VOID -- which is how Deutsch's intro came to call goToFrame on
      // nothing after its beginSprite had found the sprite perfectly well.
      if (sprite_event(d, "mousedown", d->click_on, EVENT_DOWN) ||
          (d->score[0].script &&
           frame_event(d, "mousedown", d->score[0].script, EVENT_DOWN)) ||
          dg_event(d, "mousedown", 0))
        continue;
#else
      unsigned id = d->sprites[d->click_on].value.script;
      if ((id && dg_event(d, "mousedown", id)) ||
          (d->click_on &&
           dg_event(d, "mousedown", d->sprites[d->click_on].value.member)))
        continue;
#endif
    }
    if (d->mouse_released) {
      d->mouse_released = false;
#if DG_D5
      unsigned hit=d->mouse_up_sprite;
      if(hit)d->click_on=hit;
      d->input_event=2;d->input_stage=0;d->input_channel=hit;d->pass_event=true;
      continue;
#endif
      d->mouse_up_dispatch = true;
      if (*d->mouse_up_script && dg_event(d, d->mouse_up_script, 0))
        continue;
    }
    if (d->mouse_up_dispatch) {
      d->mouse_up_dispatch = false;
      unsigned hit = dg_mouse_hit(d, d->mouse_x, d->mouse_y);
      if (hit)
        d->click_on = hit;
#if DG_MODERN
      if (sprite_event(d, "mouseup", hit, EVENT_UP) ||
          (d->score[0].script &&
           frame_event(d, "mouseup", d->score[0].script, EVENT_UP)) ||
          dg_event(d, "mouseup", 0))
        continue;
#else
      if (hit && ((d->sprites[hit].value.script &&
                   dg_event(d, "mouseup", d->sprites[hit].value.script)) ||
                  dg_event(d, "mouseup", d->sprites[hit].value.member)))
        continue;
#endif
    }
#if DG_EXTENDED
    if (d6_idle(d)) continue;
#endif
    if (d->phase == 0) {
#if DG_D5 || DG_D10
      lv_t actors=r->roots[DG_BEHAVIOR_ROOT+DG_SPRITES];
      // The actorList root stays VOID until authored code assigns it.
      if(lv_type(actors)!=LV_VOID && d->actor_cursor<lv_count(r,actors)) {
        lv_t actor=lv_at(r,actors,++d->actor_cursor);
        lv_start_method(r,actor,"stepframe",0,NULL);
        continue;
      }
#endif
#if DG_D10
      if (d10_timeouts(d))
        continue;
#endif
#if DG_MODERN
      if (!d->event_cursor) {
        d->event_cursor++;
        if (frame_event(d, "prepareframe", d->score[0].script, EVENT_PREPARE))
          continue;
        continue;
      } else if (d->event_cursor < DG_SPRITES) {
        if (dispatch_phase(d, "prepareframe", EVENT_PREPARE, &guard))
          continue;
        if (guard >= 4096)
          break;
        if (d->event_cursor < DG_SPRITES)
          continue;
      }
#endif
      d->phase = 1;
      d->event_cursor = 0;
    } else if (d->phase == 1 || d->phase == 2) {
#if DG_D5
      if(d->phase==2 && !d->event_cursor && d5_waiting(d))break;
#endif
      if (d->phase == 2 && !d->event_cursor && d->clock < d->frame_deadline)
        break;
      const char *event = d->phase == 1 ? "enterframe" : "exitframe";
#if DG_MODERN
      unsigned mask = d->phase == 1 ? EVENT_ENTER : EVENT_EXIT;
#endif
#if DG_EXTENDED
      // Deliver the event to sprite behaviors before the frame script. An
      // authored go(the frame) must not reset the dispatcher before they run.
      // Keep the cursor across yields and service-budget boundaries.
      if (!d->event_cursor) d->event_cursor = 1;
      if (d->event_cursor < DG_SPRITES) {
        if (dispatch_phase(d, event, mask, &guard)) continue;
        if (guard >= 4096) break;
        if (d->event_cursor < DG_SPRITES) continue;
      }
      if (d->event_cursor == DG_SPRITES) {
        d->event_cursor++;
        d->current_event_sprite = 0;
        if (frame_event(d, event, d->score[0].script, mask)) continue;
      }
      d->phase++;
      d->event_cursor = 0;
#else
      if (!d->event_cursor) {
        d->event_cursor++;
#if DG_MODERN
        if (event_with_mask(d, event, d->score[0].script, mask))
#else
        if (dg_event(d, event, d->score[0].script))
#endif
          continue;
      } else if (d->event_cursor < DG_SPRITES) {
#if DG_MODERN
        if (dispatch_phase(d, event, mask, &guard))
          continue;
        if (guard >= 4096)
          break;
        if (d->event_cursor >= DG_SPRITES) {
          d->phase++;
          d->event_cursor = 0;
        }
#else
        unsigned i = d->event_cursor++;
        if (d->sprites[i].value.script &&
            dg_event(d, event, d->sprites[i].value.script))
          continue;
#endif
      } else {
        d->phase++;
        d->event_cursor = 0;
      }
#endif
    } else {
      unsigned next = d->frame < d->movie->frame_count ? d->frame + 1 : 1;
      if (!advance_frame(d, next, true)) {
        if (r->failed && r->script_error)
          continue;
        break;
      }
    }
  }
  if (r->failed)
    recover_script_error(d);
  return !r->failed;
}
