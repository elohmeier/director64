/* D10 movie-in-a-window services. The corpus authors exactly one controller
 * window ("control"): the launcher loads it invisibly, navigates it between
 * controller movies, and stage code calls the window movie's interface
 * handlers whose instances live in shared globals. Keeping the window
 * movie's code resident as a shared movie reproduces that handler
 * resolution; the window's own score does not tick yet and its visible
 * statistics panel does not render yet — both remain explicit feature work,
 * traced when the game requests them. */
static const char *d10_path_base(const char *path) {
  const char *base = path;
  for (const char *p = path; *p; p++)
    if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
  return base;
}
static bool d10_span_ci(const char *span, unsigned n, const char *name) {
  for (unsigned i = 0; i < n; i++) {
    char a = span[i], b = name[i];
    if (!b) return false;
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return !name[n];
}
static bool d10_ends_ci(const char *text, const char *suffix) {
  size_t n = strlen(text), s = strlen(suffix);
  return n >= s && d10_span_ci(text + n - s, (unsigned)s, suffix);
}
// Preference files are bounded by the save archive's per-file limit. The
// same scratch serves the read-only data reads, which are self-contained.
static char d10_ini_data[16384], d10_ini_out[16640];
// Shipped read-only data keeps its authored folder tail, so an authored
// Windows path resolves by finding the data root inside it.
static bool d10_data_path(const char *path, char *out, size_t size) {
  static const char *const roots[] = {"tests_db", "hilfe", "global", "data"};
  for (unsigned i = 0; i < sizeof(roots) / sizeof(*roots); i++) {
    unsigned n = (unsigned)strlen(roots[i]);
    for (const char *p = path; *p; p++) {
      if (p != path && p[-1] != '\\' && p[-1] != '/' && p[-1] != ':')
        continue;
      if (!d10_span_ci(p, n, roots[i]) || (p[n] != '\\' && p[n] != '/'))
        continue;
      if (strlen(p) + 1 > size)
        return false;
      size_t written = 0;
      for (const char *q = p; *q; q++)
        out[written++] = *q == '\\' ? '/' : *q;
      out[written] = 0;
      return true;
    }
  }
  return false;
}
typedef struct {
  bool section_found, key_found;
  unsigned section_insert;     // offset just past the section header line
  unsigned key_start, key_end; // key line span including its newline
  const char *value;
  unsigned value_length;
} d10_ini_hit_t;
static void d10_ini_scan(const char *text, unsigned length, const char *section,
                         const char *key, d10_ini_hit_t *hit) {
  memset(hit, 0, sizeof(*hit));
  bool in_section = false;
  unsigned i = 0;
  while (i < length) {
    unsigned line = i;
    while (i < length && text[i] != '\n') i++;
    unsigned end = i;
    if (i < length) i++;
    while (end > line && (text[end - 1] == '\r' || text[end - 1] == ' ' ||
                          text[end - 1] == '\t'))
      end--;
    unsigned start = line;
    while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
    if (start >= end) continue;
    if (text[start] == '[') {
      unsigned close = start + 1;
      while (close < end && text[close] != ']') close++;
      in_section = d10_span_ci(text + start + 1, close - start - 1, section);
      if (in_section && !hit->key_found) {
        hit->section_found = true;
        hit->section_insert = i;
      }
      continue;
    }
    if (!in_section || hit->key_found) continue;
    unsigned equals = start;
    while (equals < end && text[equals] != '=') equals++;
    if (equals >= end) continue;
    unsigned key_stop = equals;
    while (key_stop > start &&
           (text[key_stop - 1] == ' ' || text[key_stop - 1] == '\t'))
      key_stop--;
    if (!d10_span_ci(text + start, key_stop - start, key)) continue;
    unsigned v = equals + 1;
    while (v < end && (text[v] == ' ' || text[v] == '\t')) v++;
    hit->key_found = true;
    hit->key_start = line;
    hit->key_end = i;
    hit->value = text + v;
    hit->value_length = end - v;
  }
}
static int d10_stem_matches(const char *stem, const char *name) {
  unsigned i = 0;
  for (; stem[i] && name[i] && name[i] != '.'; i++) {
    char a = stem[i], b = name[i];
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return 0;
  }
  return !stem[i] && (!name[i] || name[i] == '.');
}
static void d10_window_share_one(dg_runtime_t *d, const dg_movie_t *movie) {
  if (!movie) return;
  lv_runtime_t *r = d->values;
  if (movie == d->movie) return;
  for (unsigned i = 0; i < r->shared_count; i++)
    if (r->shared[i] == movie->code) return;
  if (r->shared_count >= LV_SHARED || d->loaded_count >= DG_FILES) {
    error(d, "window movie exceeds resident bounds", movie->code->name);
    return;
  }
  d->loaded[d->loaded_count++] = movie;
  r->shared[r->shared_count++] = movie->code;
}
static const dg_movie_t *d10_cast_target(dg_runtime_t *d, const char *cast_name) {
  for (unsigned i = 0; i < d->cast_swap_count; i++)
    if (eq(d->cast_swaps[i].name, cast_name)) return d->cast_swaps[i].target;
  return NULL;
}
// A score-placed member of a swapped library: the score names the authored
// archive's member, which plays as the same number in the swapped one
// (Löwenzahn's "video" cast, pointed at videoW).
static uint32_t d10_swap_member(dg_runtime_t *d, uint32_t id) {
  if (!d->cast_swap_count || !id)
    return id;
  for (unsigned c = 0; c < d->movie->cast_count; c++) {
    const dg_cast_t *lib = &d->movie->casts[c];
    if (lib->file != DG_ID_FILE(id) || lib->cast != DG_ID_CAST(id))
      continue;
    const dg_movie_t *swap = d10_cast_target(d, lib->name);
    return swap ? DG_ID(swap->id, 1u, id & 65535u) : id;
  }
  return id;
}
// castLib values 1..cast_count index the stage movie's castlist; resident
// controller-window casts encode as 256+i (home) and 512+i (window movie)
// so window code can address its own libraries from any stage.
static const dg_cast_t *d10_castlib(dg_runtime_t *d, int32_t id) {
  if (id >= 1 && (unsigned)id <= d->movie->cast_count)
    return &d->movie->casts[id - 1];
  const dg_movie_t *w = NULL;
  unsigned index = 0;
  if (id >= 512) { w = d->window_movie; index = (unsigned)id - 512; }
  else if (id >= 256) { w = d->window_home; index = (unsigned)id - 256; }
  if (w && index < w->cast_count) return &w->casts[index];
  return NULL;
}
static int32_t d10_castlib_id(dg_runtime_t *d, const char *name) {
  for (unsigned i = 0; i < d->movie->cast_count; i++)
    if (eq(d->movie->casts[i].name, name)) return (int32_t)i + 1;
  const dg_movie_t *w[2] = {d->window_home, d->window_movie};
  const int32_t bases[2] = {256, 512};
  for (unsigned n = 0; n < 2; n++) {
    if (!w[n] || w[n] == d->movie) continue;
    for (unsigned i = 0; i < w[n]->cast_count; i++)
      if (eq(w[n]->casts[i].name, name)) return bases[n] + (int32_t)i;
  }
  return 0;
}
static const char *d10_cast_store(dg_runtime_t *d, uint32_t file) {
  for (unsigned i = 0; i < d->cast_store_count; i++)
    if (d->cast_stores[i].file == file) return d->cast_stores[i].name;
  return NULL;
}
// Bind a cast archive's mutable records to a virtual save file. Loading a
// profile drops the current records and restores the file's; save-with-path
// only rebinds, then the caller writes the current records out.
static void d10_cast_store_bind(dg_runtime_t *d, const dg_cast_t *lib,
                                const char *stem, bool restore) {
  unsigned slot = d->cast_store_count;
  for (unsigned i = 0; i < d->cast_store_count; i++)
    if (d->cast_stores[i].file == lib->file) slot = i;
  if (slot == d->cast_store_count) {
    if (slot >= sizeof(d->cast_stores) / sizeof(d->cast_stores[0])) {
      error(d, "cast store capacity", stem);
      return;
    }
    d->cast_store_count++;
  }
  d->cast_stores[slot].file = lib->file;
  snprintf(d->cast_stores[slot].name, sizeof(d->cast_stores[slot].name), "%s",
           stem);
  if (restore) {
    for (unsigned i = 0; i < d->dynamic_count; i++) {
      dg_member_t *m = &d->dynamic_members[i].member;
      if (m->type && DG_ID_FILE(m->id) == lib->file) {
        d6_erase_field(d, m->id);
        *m = (dg_member_t){0};
        d->dynamic_members[i].name[0] = 0;
      }
    }
    d6_restore_cast_file(d, lib, stem);
  }
  if (d->platform.trace) {
    char text[96];
    snprintf(text, sizeof(text), "CAST_STORE %s %s", lib->name, stem);
    d->platform.trace(d->context, text);
  }
}
static void d10_window_share(dg_runtime_t *d) {
  d10_window_share_one(d, d->window_home);
  d10_window_share_one(d, d->window_movie);
  // The controller's cast archives stay resident with it: its resident code
  // resolves their members and scripts while any stage movie plays.
  const dg_movie_t *w[2] = {d->window_home, d->window_movie};
  for (unsigned n = 0; n < 2; n++) {
    if (!w[n] || !d->platform.find_movie) continue;
    for (unsigned i = 0; i < w[n]->cast_count; i++) {
      if (dg_loaded(d, w[n]->casts[i].file)) continue;
      d10_window_share_one(
          d, d->platform.find_movie(d->context, NULL, w[n]->casts[i].file));
    }
  }
  for (unsigned i = 0; i < d->cast_swap_count; i++)
    d10_window_share_one(d, d->cast_swaps[i].target);
}
static bool d10_stage_uses(const dg_runtime_t *d, const dg_movie_t *m) {
  if (d->movie == m) return true;
  for (unsigned i = 0; i < d->movie->cast_count; i++)
    if (d->movie->casts[i].file == m->id) return true;
  return false;
}
// Movies resident only for the controller window (its code and its cast
// archives) never receive another movie's event broadcast. A cast archive
// the stage also references keeps broadcasting.
static bool d10_window_only(const dg_runtime_t *d, const dg_movie_t *m) {
  if (d10_stage_uses(d, m)) return false;
  const dg_movie_t *w[2] = {d->window_home, d->window_movie};
  for (unsigned n = 0; n < 2; n++) {
    if (!w[n]) continue;
    if (w[n] == m) return true;
    for (unsigned i = 0; i < w[n]->cast_count; i++)
      if (w[n]->casts[i].file == m->id) return true;
  }
  for (unsigned i = 0; i < d->cast_swap_count; i++)
    if (d->cast_swaps[i].target == m) return true;
  return false;
}
static void d10_window_request(dg_runtime_t *d, const char *path) {
  char stem[40];
  const char *base = path;
  for (const char *p = path; *p; p++)
    if (*p == ':' || *p == '/' || *p == '\\' || *p == '@') base = p + 1;
  snprintf(stem, sizeof(stem), "%s", base);
  char *dot = strrchr(stem, '.');
  if (dot) *dot = 0;
  if (d->window_movie && d10_stem_matches(stem, d->window_movie->code->name))
    return;
  if (d->window_movie_name[0] && !d->window_movie) {
    // A load is already pending; the original loaded synchronously, so the
    // follow-up navigation queues behind it in authored order.
    snprintf(d->window_movie_queue, sizeof(d->window_movie_queue), "%s", stem);
    return;
  }
  // The platform resolves the overlay and completes dg_window_attach; until
  // then the window has a name but no resident code or ticking score.
  snprintf(d->window_movie_name, sizeof(d->window_movie_name), "%s", stem);
  d->window_movie = NULL;
  if (d->platform.trace) d->platform.trace(d->context, "WINDOW_MOVIE_LOAD_PENDING");
}
void dg_window_attach(dg_runtime_t *d, const dg_movie_t *movie) {
  d->window_movie = movie;
  if (!d->window_home) d->window_home = movie;
  if (d->platform.trace) d->platform.trace(d->context, "WINDOW_MOVIE_SCORE_PENDING");
  d10_window_share(d);
}
void dg_window_loaded(dg_runtime_t *d) {
  if (!d->movie) return;
  if (d->window_movie_name[0] && !d->window_movie &&
      d10_stem_matches(d->window_movie_name, d->movie->code->name))
    dg_window_attach(d, d->movie);
  if (d->window_movie && d->window_movie_queue[0]) {
    snprintf(d->window_movie_name, sizeof(d->window_movie_name), "%s", d->window_movie_queue);
    d->window_movie_queue[0] = 0;
    d->window_movie = NULL;
    if (d->platform.trace) d->platform.trace(d->context, "WINDOW_MOVIE_LOAD_PENDING");
  }
}
// Resolve a member or script name inside the controller window's castlist
// (with any active swap applied). cast_name narrows to one named library;
// NULL searches the whole window castlist in authored order. Returns 0 when
// no such cast exists, 1 with *id set on a hit, and 2 when a named cast
// exists but holds no member of that name (an authored #empty probe).
static int d10_window_member(dg_runtime_t *d, const char *cast_name,
                             lv_t value, uint32_t *id) {
  lv_runtime_t *r = d->values;
  bool cast_seen = false;
  const dg_movie_t *w[2] = {d->window_home, d->window_movie};
  for (unsigned n = 0; n < 2; n++) {
    if (!w[n] || w[n] == d->movie) continue;
    for (unsigned c = 0; c < w[n]->cast_count; c++) {
      const dg_cast_t *lib = &w[n]->casts[c];
      if (cast_name && !eq(lib->name, cast_name)) continue;
      cast_seen = true;
      const dg_movie_t *file = dg_loaded(d, lib->file);
      unsigned lib_cast = lib->cast;
      uint32_t lib_file = lib->file;
      const dg_movie_t *swap = d10_cast_target(d, lib->name);
      if (swap) {
        file = swap;
        lib_cast = 1;
        lib_file = swap->id;
      }
      if (lv_type(value) == LV_STRING || lv_type(value) == LV_SYMBOL) {
        if (!file) continue;
        for (unsigned i = 0; i < file->member_count; i++)
          if (file->members[i].cast == lib_cast &&
              eq(file->members[i].name, lv_cstr(r, value))) {
            *id = DG_ID(lib_file, lib_cast, file->members[i].number);
            return 1;
          }
      } else if (cast_name) {
        unsigned number = (unsigned)lv_integer(r, value);
        if (number) {
          *id = DG_ID(lib_file, lib_cast, number);
          return 1;
        }
      }
    }
  }
  return cast_seen ? 2 : 0;
}
// Fire at most one due timeout per service pass, on the 60 Hz tick clock.
static bool d10_timeouts(dg_runtime_t *d) {
  lv_runtime_t *r = d->values;
  for (unsigned n = 0; n < DG_TIMEOUTS; n++) {
    unsigned i = (d->timeout_cursor + n) % DG_TIMEOUTS;
    if (!d->timeouts[i].active || d->ticks < d->timeouts[i].next) continue;
    uint32_t step = (d->timeouts[i].period * 60u + 999u) / 1000u;
    d->timeouts[i].next = d->ticks + (step ? step : 1u);
    d->timeout_cursor = (i + 1) % DG_TIMEOUTS;
    lv_t ref = lv_make(LV_TIMEOUT, (int32_t)i + 1);
    if (lv_type(r->roots[DG_TIMEOUT_ROOT + i]) == LV_VOID) {
      for (unsigned m = 0; m < d->loaded_count; m++)
        if (lv_start(r, d->loaded[m]->code, d->timeouts[i].handler, 0, 1, &ref)) return true;
      continue;
    }
    if (lv_start_method(r, r->roots[DG_TIMEOUT_ROOT + i],
                        d->timeouts[i].handler, 1, &ref))
      return true;
  }
  return false;
}
// Named Flash depths retain their authored origin independently of the
// cropped raster bounds. The property bag exposes current timeline geometry
// until Lingo explicitly overrides a property, including a same-value write.
// Edit fields also retain their text/style descriptors.
static char d10_wrap_scratch[4096];
// Flash unescape, as the SWF-side setText applied it: the authored
// _charToUtf8 percent-encodes UTF-8 bytes, so "%c3%a4" decodes to the
// two-byte UTF-8 sequence.
static void d10_flash_unescape(const char *in, char *out, size_t size) {
  size_t w = 0;
  while (*in && w + 1 < size) {
    if (in[0] == '%' && isxdigit((unsigned char)in[1]) &&
        isxdigit((unsigned char)in[2])) {
      char pair[3] = {in[1], in[2], 0};
      out[w++] = (char)strtol(pair, NULL, 16);
      in += 3;
    } else {
      out[w++] = *in++;
    }
  }
  out[w] = 0;
}
static unsigned d10_flash_text_height(dg_runtime_t *d,
                                      const dg_flash_field_t *field,
                                      lv_t object, const char *text) {
  lv_runtime_t *r = d->values;
  int width = field->width;
  unsigned size = 0;
  if (lv_type(object) == LV_PROPLIST) {
    lv_t w = lv_get(r, NULL, "_width", object);
    if (lv_type(w) == LV_NUMBER) width = (int)lv_numeric(w);
    lv_t s = lv_get(r, NULL, "size", object);
    if (lv_type(s) == LV_NUMBER && lv_numeric(s) > 0) size = (unsigned)lv_numeric(s);
  }
  return dg_flash_field_wrap(field, size, text, width, d10_wrap_scratch,
                             sizeof(d10_wrap_scratch));
}
static const dg_film_layer_t *d10_flash_layer(const dg_member_t *m,
    const dg_sprite_t *s, const char *name) {
  if (!m || !m->film_frames || !m->film_count) return NULL;
  const dg_film_frame_t *frame = &m->film_frames[dg_film_storage_pose(m, s)];
  for (unsigned i = 0; i < frame->count; i++)
    if (frame->layers[i].name && eq(frame->layers[i].name, name))
      return &frame->layers[i];
  return NULL;
}
#include "flash_bridge.inc"
static bool d10_flash_get(dg_runtime_t *d, const char *name, lv_t owner,
                          lv_t *out) {
  if (lv_type(owner) != LV_SPRITE || lv_id(owner) < 1 || lv_id(owner) >= DG_SPRITES)
    return false;
  unsigned sprite = (unsigned)lv_id(owner);
  const dg_member_t *m = dg_member(d, d->sprites[sprite].value.member);
  if(m && m->source_xtra==1 && m->flash_clip_count) {
    lv_t object=0;dg_avm1_t *vm=d10_avm_receiver(d,owner,&object);
    if(vm){*out=d10_avm_to_lingo(d10_avm_property(d,vm,object,name));return true;}
  }
  if (!m || m->source_xtra != 1 || !*name || *name == '_')
    return false;
  lv_runtime_t *r = d->values;
  unsigned slot = d->flash_object_count;
  for (unsigned i = 0; i < d->flash_object_count; i++)
    if (d->flash_objects[i].sprite == sprite &&
        eq(d->flash_objects[i].name, name)) {
      if (d->flash_objects[i].member == m->id) {
        *out = r->roots[DG_FLASH_ROOT + i];
        return true;
      }
      slot = i;
      break;
    }
  if (slot == DG_FLASH_OBJECTS) {
    error(d, "flash object capacity", name);
    return true;
  }
  const dg_flash_field_t *field = dg_flash_field_find(m, name);
  const dg_film_layer_t *layer = d10_flash_layer(m, &d->sprites[sprite], name);
  lv_t object;
  if (field) {
    unsigned height = dg_flash_field_wrap(field, 0, field->text, field->width,
                                          d10_wrap_scratch,
                                          sizeof(d10_wrap_scratch));
    lv_t pairs[] = {
        lv_text(r, "_x", true),          lv_num(layer && layer->affine ? layer->origin_x / 65536.0 : field->x),
        lv_text(r, "_y", true),          lv_num(layer && layer->affine ? layer->origin_y / 65536.0 : field->y),
        lv_text(r, "_width", true),      lv_num(field->local_bounds[2] ? field->local_bounds[2] / 65536.0 : field->width),
        lv_text(r, "_height", true),     lv_num(field->local_bounds[3] ? field->local_bounds[3] / 65536.0 : field->height),
        lv_text(r, "_visible", true),    lv_num(1),
        lv_text(r, "_alpha", true),      lv_num(100),
        lv_text(r, "_rotation", true),   lv_num(0),
        lv_text(r, "_xscale", true),     lv_num(100),
        lv_text(r, "_yscale", true),     lv_num(100),
        lv_text(r, "size", true),        lv_num(field->style->size),
        lv_text(r, "color", true),
        lv_make(LV_COLOR, (int32_t)field->style->color),
        lv_text(r, "text", true),        lv_text(r, field->text, false),
        lv_text(r, "textheight", true),  lv_num(height),
    };
    object = lv_list(r, sizeof(pairs) / sizeof(*pairs), pairs, true);
  } else if (layer) {
    lv_t pairs[] = {
        lv_text(r, "_x", true),        lv_num(layer->origin_x / 65536.0),
        lv_text(r, "_y", true),        lv_num(layer->origin_y / 65536.0),
        lv_text(r, "_width", true),    lv_num(layer->value.width),
        lv_text(r, "_height", true),   lv_num(layer->value.height),
        lv_text(r, "_visible", true),  lv_num(1),
        lv_text(r, "_alpha", true),    lv_num(100),
        lv_text(r, "_rotation", true), lv_num(0),
        lv_text(r, "_xscale", true),   lv_num(100),
        lv_text(r, "_yscale", true),   lv_num(100),
    };
    object = lv_list(r, sizeof(pairs) / sizeof(*pairs), pairs, true);
  } else {
    error(d, "unresolved Flash timeline object", name);
    *out = (lv_t){0};
    return true;
  }
  if (r->failed)
    return true;
  if (slot == d->flash_object_count) d->flash_object_count++;
  d->flash_objects[slot].sprite = sprite;
  d->flash_objects[slot].member = m->id;
  d->flash_objects[slot].overridden = 0;
  snprintf(d->flash_objects[slot].name, sizeof(d->flash_objects[slot].name),
           "%s", name);
  r->roots[DG_FLASH_ROOT + slot] = object;
  if (d->platform.trace) {
    char text[96];
    snprintf(text, sizeof(text),
             field ? "FLASH_FIELD_OBJECT sprite=%u name=%s"
                   : "FLASH_TIMELINE_OBJECT sprite=%u name=%s",
             sprite, name);
    d->platform.trace(d->context, text);
  }
  *out = object;
  return true;
}
// The slot number plus one for a value that is a flash object bag; 0 otherwise.
static unsigned d10_flash_slot(dg_runtime_t *d, lv_t value) {
  if (lv_type(value) != LV_PROPLIST)
    return 0;
  for (unsigned i = 0; i < d->flash_object_count; i++)
    if (lv_id(d->values->roots[DG_FLASH_ROOT + i]) == lv_id(value) &&
        d->sprites[d->flash_objects[i].sprite].value.member == d->flash_objects[i].member)
      return i + 1;
  return 0;
}
static unsigned d10_flash_property_bit(const char *name) {
  static const char *const names[] = {
      "_x", "_y", "_width", "_height", "_visible", "_alpha", "_rotation", "_xscale", "_yscale"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
    if (eq(name, names[i])) return 1u << i;
  return 0;
}
static bool d10_flash_property_get(lv_runtime_t *r, lv_t object,
    const char *name, lv_t *out) {
  if(d10_avm_property_get(r,object,name,out))return true;
  dg_runtime_t *d = r->context;
  unsigned slot = d10_flash_slot(d, object), bit = d10_flash_property_bit(name);
  if (!slot || !bit) return false;
  slot--;
  if (d->flash_objects[slot].overridden & bit) return false;
  unsigned channel = d->flash_objects[slot].sprite;
  const dg_member_t *m = dg_member(d, d->sprites[channel].value.member);
  const dg_flash_field_t *field = dg_flash_field_find(m, d->flash_objects[slot].name);
  const dg_film_layer_t *layer = d10_flash_layer(m, &d->sprites[channel], d->flash_objects[slot].name);
  if (!layer) {
    if (field && !m->film_frames) return false;
    // A named depth removed by the timeline has no visible plane.
    if (bit != (1u << 4)) return false;
    *out = lv_num(0);
    return true;
  }
  if (field && !layer->affine) return false;
  double value = bit == 1 ? layer->origin_x / 65536.0
      : bit == 2 ? layer->origin_y / 65536.0
      : bit == 4 ? layer->value.width : bit == 8 ? layer->value.height
      : bit == 16 ? 1 : bit == 64 ? 0 : 100;
  unsigned flags = d->flash_objects[slot].overridden;
  if (field) {
    // TextField dimensions control local layout/reflow independently of
    // the placement transform. Registration, scale and rotation remain
    // properties of the current timeline pose until explicitly overridden.
    if (bit == 4) value = field->local_bounds[2] ? field->local_bounds[2] / 65536.0 : field->width;
    if (bit == 8) value = field->local_bounds[3] ? field->local_bounds[3] / 65536.0 : field->height;
    if (layer->affine) {
      double a = layer->matrix[0] / 65536.0, b = layer->matrix[1] / 65536.0;
      double c = layer->matrix[2] / 65536.0, e = layer->matrix[3] / 65536.0;
      if (bit == 128) value = 100 * hypot(a, b);
      if (bit == 256) value = 100 * hypot(c, e);
      if (bit == 64) value = atan2(b, a) * (180.0 / 3.14159265358979323846);
    }
  } else {
    if (bit == 4 && (flags & 128)) value *= lv_numeric(lv_get(r, NULL, "_xscale", object)) / 100;
    if (bit == 8 && (flags & 256)) value *= lv_numeric(lv_get(r, NULL, "_yscale", object)) / 100;
    if (bit == 128 && (flags & 4) && layer->value.width)
      value = 100 * lv_numeric(lv_get(r, NULL, "_width", object)) / layer->value.width;
    if (bit == 256 && (flags & 8) && layer->value.height)
      value = 100 * lv_numeric(lv_get(r, NULL, "_height", object)) / layer->value.height;
  }
  *out = lv_num(value);
  return true;
}
static void d10_flash_property_changed(lv_runtime_t *r, lv_t object, const char *name) {
  dg_runtime_t *d = r->context;
  unsigned slot = d10_flash_slot(d, object);
  if (!slot) return;
  slot--;
  unsigned bit = d10_flash_property_bit(name);
  // Width/scale are two interfaces to the same transform: the last write wins.
  unsigned other = bit == 4 ? 128 : bit == 8 ? 256 : bit == 128 ? 4 : bit == 256 ? 8 : 0;
  const dg_member_t *m = dg_member(d, d->flash_objects[slot].member);
  if (dg_flash_field_find(m, d->flash_objects[slot].name)) other = 0;
  d->flash_objects[slot].overridden = (uint16_t)((d->flash_objects[slot].overridden & ~other) | bit);
  dg_sprite_changed(d, d->flash_objects[slot].sprite);
  // The parent spec stays unchanged when one of its children moves. Its
  // rendered plane nevertheless changed, including text/style bag writes.
  d->visual_revision++;
}
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
// The XML Parser Xtra, as Birthday reads its string tables and room
// descriptions: parseString builds the document as nested property lists
// whose #name, #attributeName, #attributeValue, #child and #text answer the
// node properties the Xtra's objects do. Whitespace-only text is dropped
// (the Xtra's ignoreWhiteSpace default); entities decode; UTF-8 stays as it
// is, as every runtime string.
typedef struct {
  const char *p, *end;
  const char *error;
} d85_xml_t;
static bool d85_xml_starts(const d85_xml_t *x, const char *s) {
  size_t n = strlen(s);
  return (size_t)(x->end - x->p) >= n && !memcmp(x->p, s, n);
}
static void d85_xml_skip_to(d85_xml_t *x, const char *s) {
  size_t n = strlen(s);
  while (x->p < x->end && !d85_xml_starts(x, s)) x->p++;
  x->p = x->p + n <= x->end ? x->p + n : x->end;
}
static void d85_xml_space(d85_xml_t *x) {
  while (x->p < x->end && isspace((unsigned char)*x->p)) x->p++;
}
// Decodes [from, to) with its entities into a runtime string.
static lv_t d85_xml_string(lv_runtime_t *r, const char *from, const char *to) {
  static char text[32768];
  size_t n = 0;
  while (from < to && n + 5 < sizeof(text)) {
    if (*from != '&') { text[n++] = *from++; continue; }
    const char *semi = memchr(from, ';', (size_t)(to - from));
    if (!semi || semi - from > 10) { text[n++] = *from++; continue; }
    size_t len = (size_t)(semi - from) - 1;
    const char *name = from + 1;
    unsigned code = 0;
    if (len == 3 && !memcmp(name, "amp", 3)) code = '&';
    else if (len == 2 && !memcmp(name, "lt", 2)) code = '<';
    else if (len == 2 && !memcmp(name, "gt", 2)) code = '>';
    else if (len == 4 && !memcmp(name, "quot", 4)) code = '"';
    else if (len == 4 && !memcmp(name, "apos", 4)) code = '\'';
    else if (len > 1 && name[0] == '#')
      code = (unsigned)strtoul(name + 1 + (name[1] == 'x'), NULL, name[1] == 'x' ? 16 : 10);
    if (!code) { text[n++] = *from++; continue; }
    // UTF-8 for a numeric reference.
    if (code < 0x80) text[n++] = (char)code;
    else if (code < 0x800) { text[n++] = (char)(0xc0 | code >> 6); text[n++] = (char)(0x80 | (code & 63)); }
    else { text[n++] = (char)(0xe0 | code >> 12); text[n++] = (char)(0x80 | ((code >> 6) & 63)); text[n++] = (char)(0x80 | (code & 63)); }
    from = semi + 1;
  }
  text[n] = 0;
  return lv_text(r, text, false);
}
static lv_t d85_xml_node(lv_runtime_t *r, lv_t name, lv_t attribute_names, lv_t attribute_values,
                         lv_t children, lv_t text) {
  lv_t pairs[10] = {lv_symbol(r, "name"), name, lv_symbol(r, "attributename"), attribute_names,
                    lv_symbol(r, "attributevalue"), attribute_values, lv_symbol(r, "child"), children,
                    lv_symbol(r, "text"), text};
  return lv_list(r, 10, pairs, true);
}
static lv_t d85_xml_element(lv_runtime_t *r, d85_xml_t *x, unsigned depth) {
  if (depth > 32 || x->p >= x->end || *x->p != '<') { x->error = "element expected"; return (lv_t){0}; }
  x->p++;
  const char *start = x->p;
  while (x->p < x->end && !isspace((unsigned char)*x->p) && *x->p != '/' && *x->p != '>') x->p++;
  lv_t name = d85_xml_string(r, start, x->p);
  lv_t keys[64], values[128];
  unsigned attributes = 0;
  bool closed = false;
  for (;;) {
    d85_xml_space(x);
    if (x->p >= x->end) { x->error = "unterminated tag"; return (lv_t){0}; }
    if (*x->p == '/') { closed = true; x->p++; d85_xml_skip_to(x, ">"); break; }
    if (*x->p == '>') { x->p++; break; }
    const char *key = x->p;
    while (x->p < x->end && *x->p != '=' && !isspace((unsigned char)*x->p)) x->p++;
    const char *key_end = x->p;
    d85_xml_space(x);
    if (x->p >= x->end || *x->p != '=') { x->error = "attribute without value"; return (lv_t){0}; }
    x->p++;
    d85_xml_space(x);
    char quote = x->p < x->end ? *x->p : 0;
    if (quote != '"' && quote != '\'') { x->error = "unquoted attribute"; return (lv_t){0}; }
    const char *value = ++x->p;
    while (x->p < x->end && *x->p != quote) x->p++;
    if (attributes < 64) {
      keys[attributes] = d85_xml_string(r, key, key_end);
      values[2 * attributes] = keys[attributes];
      values[2 * attributes + 1] = d85_xml_string(r, value, x->p);
      attributes++;
    }
    if (x->p < x->end) x->p++;
  }
  // Children collect in heap scratch: a room lists hundreds.
  unsigned capacity = 16, count = 0;
  lv_t *children = malloc(capacity * sizeof(lv_t));
  while (!closed && children && !x->error) {
    if (x->p >= x->end) { x->error = "unterminated element"; break; }
    if (d85_xml_starts(x, "</")) { d85_xml_skip_to(x, ">"); break; }
    lv_t child = {0};
    bool have = false;
    if (d85_xml_starts(x, "<!--")) { d85_xml_skip_to(x, "-->"); continue; }
    if (d85_xml_starts(x, "<?")) { d85_xml_skip_to(x, "?>"); continue; }
    if (d85_xml_starts(x, "<![CDATA[")) {
      x->p += 9;
      const char *data = x->p;
      d85_xml_skip_to(x, "]]>");
      child = d85_xml_node(r, lv_text(r, "", false), lv_list(r, 0, NULL, false), lv_list(r, 0, NULL, true),
                           lv_list(r, 0, NULL, false), lv_bytes(r, data, (unsigned)(x->p - 3 - data)));
      have = true;
    } else if (*x->p == '<') {
      child = d85_xml_element(r, x, depth + 1);
      have = !x->error;
    } else {
      const char *text = x->p;
      bool blank = true;
      for (; x->p < x->end && *x->p != '<'; x->p++)
        if (!isspace((unsigned char)*x->p)) blank = false;
      if (!blank) {
        child = d85_xml_node(r, lv_text(r, "", false), lv_list(r, 0, NULL, false), lv_list(r, 0, NULL, true),
                             lv_list(r, 0, NULL, false), d85_xml_string(r, text, x->p));
        have = true;
      }
    }
    if (!have) continue;
    if (count == capacity) {
      lv_t *grown = realloc(children, 2 * capacity * sizeof(lv_t));
      if (!grown) break;
      children = grown;
      capacity *= 2;
    }
    children[count++] = child;
  }
  lv_t list = lv_list(r, children ? count : 0, children, false);
  free(children);
  return d85_xml_node(r, name, lv_list(r, attributes, keys, false), lv_list(r, 2 * attributes, values, true),
                      list, (lv_t){0});
}
#endif
// QuickTime and DirectMedia video sprites use 600ths of a second internally.
// Their public APIs differ: QuickTime duration/movieTime use ticks (60 Hz),
// while DirectMedia duration/currentTime/videoSeek use milliseconds.
static void d10_video_init(dg_runtime_t *d, dg_sprite_t *s) {
  if (s->media_member == s->value.member)
    return;
  const dg_member_t *m = dg_member(d, s->value.member);
  s->media_member = s->value.member;
  s->media_frame = UINT32_MAX;
  s->video_time = 0;
  s->video_start = 0;
  s->video_stop = 0;
  s->video_volume = 255;
  s->video_serial++;
  s->video_rate = dg_is_video(m) && m->samples && !(m->video_flags & 256) ? 1 : 0;
}
static void d10_video_tick(dg_runtime_t *d) {
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    dg_sprite_t *s = &d->sprites[i];
    if (!s->value.type && !s->media_member)
      continue;
    d10_video_init(d, s);
    const dg_member_t *m = s->value.type ? dg_member(d, s->value.member) : NULL;
    if (m && m->source_xtra == 1 && m->film_count) {
      flash_tick(d,s,m);
      if (m->flash_direct_to_stage && flash_rate(d,m,s,NULL) && flash_advance(d,s,m))
        dg_sprite_changed(d,i);
      continue;
    }
    if (!dg_is_video(m) || !s->video_rate)
      continue;
    const double stop = s->video_stop && s->video_stop < m->samples ? s->video_stop : m->samples;
    const double start = s->video_start < stop ? s->video_start : 0;
    s->video_time += 10 * s->video_rate;
    if (s->video_time >= stop || s->video_time < start) {
      if (member_looping(d, m) && stop > start) {
        double offset = fmod(s->video_time - start, stop - start);
        if (offset < 0) offset += stop - start;
        s->video_time = start + offset;
        s->video_serial++;
      } else {
        s->video_time = s->video_time < start ? start : stop;
        s->video_rate = 0;
      }
    }
    dg_sprite_changed(d, i);
  }
}
static void d10_video_seek(dg_sprite_t *s, const dg_member_t *m, double time) {
  s->video_time = fmin(fmax(0, time), m->samples);
  s->video_serial++;
}
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
// the systemDate: the device's local date as a date's day, month and year
// (the Adventskalender opens its doors by it; players can change it).
static lv_t d85_system_date(lv_runtime_t *r) {
  time_t now = time(NULL);
  struct tm local = *localtime(&now);
  lv_t pairs[6] = {lv_symbol(r, "year"), lv_num(local.tm_year + 1900), lv_symbol(r, "month"),
                   lv_num(local.tm_mon + 1), lv_symbol(r, "day"), lv_num(local.tm_mday)};
  return lv_list(r, 6, pairs, true);
}
#endif
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
// the timeoutScript: once timeoutLength ticks pass without a click or a
// key, the named movie handler runs, and the wait starts over (Löwenzahn
// 8's yeti game nudges an idle player this way).
static bool d85_idle_timeout(dg_runtime_t *d) {
  if (!d->timeout_script[0] || !d->timeout_length || d->ticks - d->last_input_tick < d->timeout_length)
    return false;
  d->last_input_tick = d->ticks;
  for (unsigned i = 0; i < d->loaded_count; i++)
    if (lv_start(d->values, d->loaded[i]->code, d->timeout_script, 0, 0, NULL)) return true;
  return false;
}
// A text member's live text: a field's rewritten text, else the authored.
static const char *d85_member_text(dg_runtime_t *d, const dg_member_t *m) {
  for (unsigned j = 0; j < d->field_count; j++)
    if (d->field_members[j] == m->id)
      return lv_cstr(d->values, d->values->roots[DG_FIELD_TEXT_ROOT + j]);
  return m->text ? m->text : "";
}
// The byte where Unicode character `index` of UTF-8 text starts.
static size_t d85_char_byte(const char *text, int index) {
  size_t at = 0;
  for (int n = 0; text[at] && n < index; n++)
    for (at++; ((unsigned char)text[at] & 0xc0) == 0x80; at++) {
    }
  return at;
}
static int d85_byte_char(const char *text, size_t byte) {
  int n = 0;
  for (size_t at = 0; at < byte && text[at]; at++)
    n += ((unsigned char)text[at] & 0xc0) != 0x80;
  return n;
}
// The palette entry nearest an RGB colour, as an 8-bit text colour reads.
static unsigned d85_nearest_index(dg_runtime_t *d, uint32_t rgb) {
  unsigned best = 255;
  uint32_t best_distance = UINT32_MAX;
  for (unsigned i = 0; d->movie && dg_stage_palette(d) && i < 256; i++) {
    uint32_t p = dg_stage_palette(d)[i];
    int dr = (int)(p >> 16 & 255) - (int)(rgb >> 16 & 255), dg = (int)(p >> 8 & 255) - (int)(rgb >> 8 & 255),
        db = (int)(p & 255) - (int)(rgb & 255);
    uint32_t distance = (uint32_t)(dr * dr + dg * dg + db * db);
    if (distance < best_distance) best_distance = distance, best = i;
  }
  return best;
}
// The character of a text sprite at a stage point, -1 for none.
static int d85_sprite_char(dg_runtime_t *d, unsigned channel, lv_t point) {
  if (!channel || channel >= DG_SPRITES || !d->platform.text_char_at || lv_type(point) != LV_LIST)
    return -1;
  const dg_member_t *m = dg_member(d, d->sprites[channel].value.member);
  if (!m || (m->type != 3 && m->type != 7))
    return -1;
  int l, t, rr, b;
  dg_sprite_bounds(d, &d->sprites[channel], &l, &t, &rr, &b);
  return d->platform.text_char_at(d->context, m->id, channel, lv_integer(d->values, lv_at(d->values, point, 1)) - l,
                                  lv_integer(d->values, lv_at(d->values, point, 2)) - t);
}
// A playlist entry names its sound as a member or as a [#member: ...] list.
static uint32_t d85_sound_entry(dg_runtime_t *d, lv_t entry) {
  if (lv_type(entry) == LV_PROPLIST) entry = lv_get(d->values, NULL, "member", entry);
  return lv_type(entry) == LV_MEMBER ? (uint32_t)lv_id(entry) : 0;
}
static void d85_sound_start(dg_runtime_t *d, unsigned channel, uint32_t id) {
  const dg_member_t *m = dg_member(d, id);
  if (!m || m->type != 6) {
    if (d->platform.trace) d->platform.trace(d->context, "SOUND_PLAY_EMPTY");
    return;
  }
  d->sound_puppet[channel] = true;
  d->sound_serial[channel]++;
  play_member(d, channel, m);
}
static void d85_sound_queue(dg_runtime_t *d, unsigned channel, uint32_t id) {
  if (id && d->sound_queue_count[channel] < DG_SOUND_QUEUE)
    d->sound_queue[channel][d->sound_queue_count[channel]++] = id;
}
static bool d85_sound_busy(dg_runtime_t *d, unsigned channel) {
  return d->platform.sound_busy && d->platform.sound_busy(d->context, channel);
}
static void d85_sound_next(dg_runtime_t *d, unsigned channel) {
  uint32_t id = d->sound_queue[channel][0];
  memmove(&d->sound_queue[channel][0], &d->sound_queue[channel][1],
          sizeof(d->sound_queue[0][0]) * --d->sound_queue_count[channel]);
  d85_sound_start(d, channel, id);
}
// A playing channel moves on to its next queued sound once one ends.
static void d85_sound_service(dg_runtime_t *d) {
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) {
    if (!d->sound_chain[ch] || d85_sound_busy(d, ch)) continue;
    if (d->sound_queue_count[ch]) d85_sound_next(d, ch);
    else d->sound_chain[ch] = false;
  }
}
#endif
// Settings the 8.5 projectors read back that change nothing on the target.
static int32_t d10_idle_period;
static bool d10_get(dg_runtime_t *d, const char *name, lv_t owner, lv_t *out) {
  if (flash_playback_property(d,name,owner,out,false)) return true;
  if (linked_get(d, name, owner, out)) return true;
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
  if (lv_type(owner) == LV_SPRITE && ((ln == LN_MEDIABUSY) || (ln == LN_MOVIERATE) ||
                                      (ln == LN_CURRENTTIME) || (ln == LN_MOVIETIME) || (ln == LN_STARTTIME) ||
                                      (ln == LN_STOPTIME) || (ln == LN_VOLUME))) {
    dg_sprite_t *s = sprite(d, owner);
    const dg_member_t *m = s && s->value.type ? dg_member(d, s->value.member) : NULL;
    if (!dg_is_video(m)) {
      *out = lv_num(0);
      return true;
    }
    d10_video_init(d, s);
    if (ln == LN_MEDIABUSY)
      *out = lv_num(s->video_rate != 0 && s->video_time < m->samples);
    else if (ln == LN_MOVIERATE)
      *out = lv_num(s->video_rate);
    else if (ln == LN_VOLUME)
      *out = lv_num(s->video_volume);
    else if (ln == LN_STARTTIME || ln == LN_STOPTIME)
      *out = lv_num((ln == LN_STARTTIME ? s->video_start : s->video_stop) / 10);
    else if (ln == LN_CURRENTTIME)
      *out = lv_num(floor(s->video_time * 10 / 6));
    else
      *out = lv_num(floor(s->video_time / 10));
    return true;
  }
  if (lv_type(owner) == LV_MEMBER &&
      ((ln == LN_DURATION) || (ln == LN_VIDEOWIDTH) || (ln == LN_VIDEOHEIGHT))) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (dg_is_video(m)) {
      *out = lv_num(ln == LN_DURATION ? (m->source_xtra == 3 ? floor(m->samples * 10.0 / 6) : floor(m->samples / 10.0))
                    : ln == LN_VIDEOWIDTH ? m->width : m->height);
      return true;
    }
  }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  if (lv_type(owner) == LV_XTRA && lv_id(owner) == 5 &&
      ((ln == LN_AMPVALUE) || (ln == LN_RESOLUTION))) {
    if (ln == LN_RESOLUTION) { *out = lv_num(d->amp_resolution); return true; }
    lv_t curve = r->roots[DG_AMP_ROOT];
    unsigned ch = d->amp_channel ? d->amp_channel - 1u : 0u, value = 0;
    if (d->amp_channel && lv_type(curve) == LV_LIST && d->platform.sound_position &&
        d->platform.sound_busy && d->platform.sound_busy(d->context, ch)) {
      unsigned index = d->platform.sound_position(d->context, ch) * d->amp_resolution / 1000u + 1u;
      if (index <= lv_count(r, curve)) value = (unsigned)lv_integer(r, lv_at(r, curve, index));
    }
    *out = lv_num((int32_t)value);
    return true;
  }
#endif
  // A script member's scriptType, from the kind its handlers were
  // converted as (Birthday tells its animation parent scripts apart).
  if (lv_type(owner) == LV_MEMBER && (ln == LN_SCRIPTTYPE)) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    const dg_movie_t *movie = m ? dg_loaded(d, DG_ID_FILE(m->id)) : NULL;
    const char *kind = NULL;
    for (unsigned i = 0; movie && m->cast && m->cast <= movie->cast_count && i < movie->code->count; i++) {
      const lv_handler_t *h = &movie->code->handlers[i];
      if (h->member == m->number && h->cast && eq(h->cast, movie->casts[m->cast - 1].name)) {
        kind = h->kind;
        break;
      }
    }
    *out = !kind ? (lv_t){0}
           : lv_symbol(r, strstr(kind, "Movie") ? "movie" : strstr(kind, "Parent") ? "parent" : "score");
    return true;
  }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // A text member's hyperlinks: [[first, last], ...] in characters.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_HYPERLINKS)) {
    const dg_text_extra_t *extra = dg_text_extra(d, (uint32_t)lv_id(owner));
    lv_t ranges[255];
    unsigned count = extra ? (extra->link_count < 255 ? extra->link_count : 255) : 0;
    for (unsigned i = 0; i < count; i++) {
      lv_t pair[2] = {lv_num((int32_t)extra->links[i].first), lv_num((int32_t)extra->links[i].last)};
      ranges[i] = lv_list(r, 2, pair, false);
    }
    *out = lv_list(r, count, ranges, false);
    return true;
  }
#endif
  // An rgb color's components (Löwenzahn 6's bat cave fades by them).
  if (lv_type(owner) == LV_COLOR && ((ln == LN_RED) || (ln == LN_GREEN) || (ln == LN_BLUE))) {
    uint32_t rgb = (uint32_t)lv_id(owner);
    *out = lv_num((int32_t)(rgb >> (ln == LN_RED ? 16 : ln == LN_GREEN ? 8 : 0) & 255));
    return true;
  }
  // A sound's authored cue points, in milliseconds and by name (Circus' hen
  // dance times its letters from them).
  if (lv_type(owner) == LV_MEMBER && (ln == LN_CUEPOINTTIMES || ln == LN_CUEPOINTNAMES)) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    lv_t items[255];
    unsigned count = m ? m->cue_count : 0;
    for (unsigned i = 0; i < count; i++)
      items[i] = ln == LN_CUEPOINTTIMES ? lv_num((int32_t)m->cues[i].milliseconds)
                                        : lv_text(r, m->cues[i].name ? m->cues[i].name : "", false);
    *out = lv_list(r, count, items, false);
    return true;
  }
  // A script member's script object.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_SCRIPT)) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (m && m->type == 11) { *out = lv_make(LV_SCRIPT, lv_id(owner)); return true; }
  }
  // A text member's alignment, from its converted style.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_ALIGNMENT)) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (m && (m->type == 3 || m->type == 7)) {
      unsigned align = m->text_style ? m->text_style->align : 0;
      *out = lv_text(r, align == 1 ? "center" : align == 2 ? "right" : "left", true);
      return true;
    }
  }
  // Transition members play as cuts (puppetTransition), so they take no time.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_DURATION)) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (m && m->type == 14) { *out = lv_num(0); return true; }
    // A sound's length in milliseconds (Circus scales its lip-sync lists).
    if (m && m->type == 6 && m->rate) {
      *out = lv_num((int32_t)((uint64_t)m->samples * 1000u / m->rate));
      return true;
    }
  }
  if (lv_type(owner) == LV_VOID) {
    if (ln == LN_PARAMCOUNT) { *out = lv_num(lv_param_count(r)); return true; }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    // The Xtras these ports answer for: Löwenzahn's start movie looks for
    // DirectMedia ("DIRECTME") before it plays its videos.
    if (ln == LN_XTRALIST) {
      static const char *const files[] = {"DIRECTME.X32", "QTASSET.X32", "FILEIO.X32", "BUDAPI.X32"};
      lv_t items[4];
      for (unsigned i = 0; i < 4; i++) {
        lv_t pair[4] = {lv_symbol(r, "name"), lv_text(r, files[i], false), lv_symbol(r, "filename"),
                        lv_text(r, files[i], false)};
        items[i] = lv_list(r, 4, pair, true);
      }
      *out = lv_list(r, 4, items, false);
      return true;
    }
#endif
    if (ln == LN_IDLEHANDLERPERIOD) { *out = lv_num(d10_idle_period); return true; }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    if (ln == LN_TIMEOUTLENGTH) { *out = lv_num((int32_t)d->timeout_length); return true; }
    if (ln == LN_TIMEOUTSCRIPT) { *out = lv_text(r, d->timeout_script, false); return true; }
    if (ln == LN_TIMEOUTLAPSED) { *out = lv_num((int32_t)(d->ticks - d->last_input_tick)); return true; }
#endif
    if (ln == LN_LASTFRAME) { *out = lv_num(d->movie ? d->movie->frame_count : 0); return true; }
    // The Windows projectors' sound mixer; lip-sync code branches on MacroMix.
    if (ln == LN_SOUNDDEVICE) { *out = lv_text(r, "DirectSound", false); return true; }
    if (ln == LN_SOUNDDEVICELIST) {
      lv_t devices[2] = {lv_text(r, "MacroMix", false), lv_text(r, "DirectSound", false)};
      *out = lv_list(r, 2, devices, false);
      return true;
    }
    if (ln == LN_APPLICATIONNAME) { *out = lv_text(r, "Projector.exe", false); return true; }
    if ((ln == LN_DATE) || (ln == LN_SHORTDATE)) { *out = lv_text(r, "11.10.2026", false); return true; }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    if (ln == LN_SYSTEMDATE) { *out = d85_system_date(r); return true; }
#endif
    if (ln == LN_CLICKLOC) { *out = lv_point(r, d->mouse_x, d->mouse_y); return true; }
    if ((ln == LN_WINDOWLIST) || (ln == LN_SCORESELECTION)) { *out = lv_list(r, 0, NULL, false); return true; }
    if ((ln == LN_FRAMESOUND1) || (ln == LN_FRAMESOUND2)) {
      uint32_t id = d->score[ln == LN_FRAMESOUND1 ? 4 : 3].member;
      unsigned cast = id ? member_cast(d, id) : 0;
      *out = lv_num(id ? ((cast ? cast - 1 : 0) << 17) | (id & 65535) : 0);
      return true;
    }
    // Fixed environment answers (8 MiB of memory, 4 MiB free); the memory
    // monitor renders them, it never branches on them.
    if (ln == LN_MEMORYSIZE) { *out = lv_num(8 * 1024 * 1024); return true; }
    if ((ln == LN_FREEBYTES) || (ln == LN_FREEBLOCK)) {
      *out = lv_num(4 * 1024 * 1024);
      return true;
    }
    if (ln == LN_ACTORLIST) {
      // Director starts with an empty actor list that scripts add to.
      if (lv_type(r->roots[DG_BEHAVIOR_ROOT + DG_SPRITES]) == LV_VOID)
        r->roots[DG_BEHAVIOR_ROOT + DG_SPRITES] = lv_list(r, 0, NULL, false);
      *out = r->roots[DG_BEHAVIOR_ROOT + DG_SPRITES];
      return true;
    }
    if (ln == LN_PLATFORM) {
      // The source projector ran under Windows; authored checks branch on it.
      *out = lv_text(r, "Windows,32", false);
      return true;
    }
    if ((ln == LN_LONGTIME) || (ln == LN_SHORTTIME) ||
        (ln == LN_TIME)) {
      // Authored logging timestamps; a wall clock derived from the session
      // tick counter keeps successive prints ordered without an RTC.
      unsigned seconds = d->ticks / 60u;
      char text[16];
      if (ln == LN_LONGTIME)
        snprintf(text, sizeof(text), "%u:%02u:%02u", 10 + seconds / 3600u % 12u,
                 seconds / 60u % 60u, seconds % 60u);
      else
        snprintf(text, sizeof(text), "%u:%02u", 10 + seconds / 3600u % 12u,
                 seconds / 60u % 60u);
      *out = lv_text(r, text, false);
      return true;
    }
    if (ln == LN_GLOBALS) {
      *out = lv_make(LV_GLOBALS_VIEW, 0);
      return true;
    }
    if (ln == LN_ROLLOVER) {
      *out = lv_num(dg_mouse_hit(d, d->mouse_x, d->mouse_y));
      return true;
    }
    // The authored stage is 800x600; the platform prescales at its render
    // boundary. Scripts must observe the authored geometry.
    if (ln == LN_STAGERIGHT) { *out = lv_num(800); return true; }
    if (ln == LN_STAGEBOTTOM) { *out = lv_num(600); return true; }
    if (ln == LN_STAGE) {
      // The stage's image is image 0: assigned to a member, it becomes the
      // platform's snapshot of what the stage shows (d10_set).
      lv_t rect = lv_rect(r, 0, 0, 800, 600);
      *out = lv_list(r, 6,
                     (lv_t[]){lv_text(r, "rect", true), rect,
                              lv_text(r, "sourcerect", true), rect,
                              lv_text(r, "image", true), lv_make(LV_IMAGE, 0)},
                     true);
      return true;
    }
    if (ln == LN_DESKTOPRECTLIST) {
      *out = lv_list(r, 1, (lv_t[]){lv_rect(r, 0, 0, 800, 600)}, false);
      return true;
    }
    if (ln == LN_TIMEOUTLIST) {
      lv_t entries[DG_TIMEOUTS];
      unsigned active = 0;
      for (unsigned i = 0; i < DG_TIMEOUTS; i++)
        if (d->timeouts[i].active)
          entries[active++] = lv_make(LV_TIMEOUT, (int32_t)i + 1);
      *out = lv_list(r, active, entries, false);
      return true;
    }
    if (ln == LN_SCRIPTINGXTRALIST) {
      // No scripting Xtras exist on the target; presence probes answer no.
      *out = lv_list(r, 0, NULL, false);
      return true;
    }
    if (ln == LN_FRAMETEMPO) {
      *out = lv_num(d->tempo);
      return true;
    }
    if (ln == LN_NUMBEROFCASTLIBS) {
      *out = lv_num(d->movie->cast_count);
      return true;
    }
    if (ln == LN_LASTCHANNEL) {
      // Authored channel sweeps probe every score slot; channels without a
      // member answer #empty, so the runtime capacity bounds the sweep.
      *out = lv_num(DG_SPRITES - 1);
      return true;
    }
    return false;
  }
  if (lv_type(owner) == LV_MEMBER && (ln == LN_VERTEXLIST)) {
    // Converted vector shapes ship as rasterized artwork, so their outline
    // is fixed. The list answers the member's own rectangle, which is what
    // the exercises measure before writing a resized one back.
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (!m)
      return false;
    double w = m->width / 2.0, h = m->height / 2.0;
    const double xs[4] = {-w, w, w, -w}, ys[4] = {-h, -h, h, h};
    lv_t corners[4];
    for (unsigned i = 0; i < 4; i++) {
      lv_t pair[] = {lv_text(r, "vertex", true), lv_point(r, xs[i], ys[i])};
      corners[i] = lv_list(r, 2, pair, true);
    }
    *out = lv_list(r, 4, corners, false);
    return true;
  }
  if (lv_type(owner) == LV_SPRITE &&
      ((ln == LN__ROOT) || (ln == LN__LEVEL0))) {
    // The Flash sprite stands in for its own timeline root: authored code
    // reaches fields both ways (pSprite.my_txt and pSprite._root.my_txt).
    *out = owner;
    return true;
  }
  if (lv_type(owner) == LV_SPRITE && (ln == LN_FRAME)) {
    // The Flash sprite's position on its flattened timeline, counted from
    // one as the player did; a sprite without one answers zero, which is
    // what the authored code tests before seeking.
    unsigned n = (unsigned)lv_id(owner);
    const dg_member_t *m = n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
    *out = lv_num(m && m->film_count ? dg_film_pose(m, &d->sprites[n]) + 1 : 0);
    return true;
  }
  if (lv_type(owner) == LV_SPRITE && (ln == LN_PLAYING)) {
    // A flattened Flash timeline reports playing until its first pass ends;
    // the visual keeps looping while authored gates observe completion.
    unsigned n = (unsigned)lv_id(owner);
    const dg_member_t *m = n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
    *out = lv_num(m && m->film_count > 1 && !d->sprites[n].film_paused &&
                  (m->film_loop || d->sprites[n].film_frame + 1 < m->film_count));
    return true;
  }
  if (lv_type(owner) == LV_CASTLIB && (ln == LN_FILENAME)) {
    const dg_cast_t *lib = d10_castlib(d, lv_id(owner));
    if (!lib) {
      error(d, "cast library range", name);
      return true;
    }
    const char *store = d10_cast_store(d, lib->file);
    if (store) {
      *out = lv_text(r, store, false);
      return true;
    }
    const dg_movie_t *swap = d10_cast_target(d, lib->name);
    if (swap) {
      for (unsigned i = 0; i < d->cast_swap_count; i++)
        if (eq(d->cast_swaps[i].name, lib->name)) {
          *out = lv_text(r, d->cast_swaps[i].stem, false);
          return true;
        }
    }
    const dg_movie_t *file = dg_loaded(d, lib->file);
    *out = lv_text(r, file ? file->code->name : "", false);
    return true;
  }
  if (lv_type(owner) == LV_CASTLIB && (ln == LN_NUMBEROFMEMBERS)) {
    const dg_cast_t *lib = d10_castlib(d, lv_id(owner));
    if (!lib) {
      error(d, "cast library range", name);
      return true;
    }
    // The highest member number across the archive's static and mutable
    // records; authored erase sweeps run 1 through this value.
    const dg_movie_t *swap = d10_cast_target(d, lib->name);
    const dg_movie_t *file = swap ? swap : dg_loaded(d, lib->file);
    unsigned lib_cast = swap ? 1 : lib->cast;
    uint32_t lib_file = swap ? swap->id : lib->file;
    unsigned highest = 0;
    if (file)
      for (unsigned i = 0; i < file->member_count; i++)
        if (file->members[i].cast == lib_cast &&
            file->members[i].number > highest)
          highest = file->members[i].number;
    for (unsigned i = 0; i < d->dynamic_count; i++) {
      const dg_member_t *m = &d->dynamic_members[i].member;
      if (m->type && DG_ID_FILE(m->id) == lib_file && m->cast == lib_cast &&
          m->number > highest)
        highest = m->number;
    }
    *out = lv_num(highest);
    return true;
  }
  if (lv_type(owner) == LV_SPRITE &&
      ((ln == LN_ENDFRAME) || (ln == LN_STARTFRAME))) {
    // The channel's current score span: sprite(0) is the frame-script
    // channel, whose bounds drive authored hold-and-loop behaviors.
    unsigned channel = lv_id(owner) ? (unsigned)lv_id(owner) + 5u : 0u;
    unsigned start = 1;
    unsigned end = d->movie->frame_count ? d->movie->frame_count : 1;
    for (unsigned f = 1; f <= d->movie->frame_count; f++) {
      const dg_frame_t *frame_entry = &d->movie->frames[f - 1];
      bool writes = false;
      for (unsigned n = 0; n < frame_entry->count && !writes; n++)
        writes = d->movie->deltas[frame_entry->first + n].channel == channel;
      if (!writes) continue;
      if (f <= d->frame) {
        start = f;
      } else {
        end = f - 1;
        break;
      }
    }
    *out = lv_num(name[0] == 'e' ? end : start);
    return true;
  }
  if (lv_type(owner) == LV_MEMBER && (ln == LN_FRAMECOUNT)) {
    // Flattened Flash timelines answer their frame span; a deferred Flash
    // member has no playable frames and reports a single held frame.
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    *out = lv_num(m && m->film_count ? m->film_count : 1);
    return true;
  }
  if (lv_type(owner) == LV_TIMEOUT) {
    unsigned slot = (unsigned)lv_id(owner) - 1;
    if (slot >= DG_TIMEOUTS) return false;
    if (ln == LN_PERSISTENT) { *out = lv_num(d->timeouts[slot].persistent); return true; }
    if (ln == LN_PERIOD) { *out = lv_num(d->timeouts[slot].period); return true; }
    // The milliSeconds at which it next fires.
    if (ln == LN_TIME) {
      *out = lv_num((int32_t)((uint64_t)d->timeouts[slot].next * 1000u / 60u));
      return true;
    }
    if (ln == LN_NAME) { *out = lv_text(r, d->timeouts[slot].name, false); return true; }
    if (ln == LN_TARGET) { *out = r->roots[DG_TIMEOUT_ROOT + slot]; return true; }
    if (ln == LN_FORGET) {
      d->timeouts[slot].active = false;
      r->roots[DG_TIMEOUT_ROOT + slot] = (lv_t){0};
      *out = (lv_t){0};
      return true;
    }
  }
  if (imaging_get(d, name, owner, out)) return true;
  if (lv_type(owner) != LV_WINDOW) return false;
  if (ln == LN_VISIBLE) { *out = lv_num(d->window_visible); return true; }
  if ((ln == LN_FILENAME) || (ln == LN_NAME)) {
    *out = lv_text(r, d->window_movie_name, false);
    return true;
  }
  if ((ln == LN_RECT) || (ln == LN_DRAWRECT)) {
    *out = lv_rect(r, d->window_rect[0], d->window_rect[1], d->window_rect[2], d->window_rect[3]);
    return true;
  }
  // Property-form window actions: window("control").forget and .open.
  if (ln == LN_FORGET) { d->window_open = d->window_visible = false; *out = (lv_t){0}; return true; }
  if ((ln == LN_OPEN) || (ln == LN_MOVETOFRONT)) {
    d->window_open = true;
    if (d->platform.trace && d->window_visible)
      d->platform.trace(d->context, "WINDOW_PANEL_PENDING_RENDER");
    *out = (lv_t){0};
    return true;
  }
  return false;
}
static bool d10_set(dg_runtime_t *d, const char *name, lv_t owner, lv_t value) {
  if (flash_playback_property(d,name,owner,&value,true)) return true;
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  if (lv_type(owner) == LV_XTRA && lv_id(owner) == 5 && (ln == LN_RESOLUTION)) {
    int32_t resolution = lv_integer(r, value);
    d->amp_resolution = (uint16_t)(resolution > 0 && resolution < 1000 ? resolution : 15);
    return true;
  }
#endif
  // Renaming a member: the converted tables index members by their authored
  // names, so only a name it already has is taken (Löwenzahn 8's phantom
  // picture names its image members "1".."15" again); others are traced.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_NAME)) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (!(m && m->name && eq(m->name, lv_cstr(r, value))) && d->platform.trace)
      d->platform.trace(d->context, "MEMBER_RENAME_PENDING");
    return true;
  }
  if (lv_type(owner) == LV_VOID) {
    if (ln == LN_IDLEHANDLERPERIOD) { d10_idle_period = (int32_t)lv_integer(r, value); return true; }
    // Authoring and mixer settings with nothing to change on the target.
    if ((ln == LN_DEBUGPLAYBACKENABLED) || (ln == LN_SOUNDDEVICE) || (ln == LN_SOUNDKEEPDEVICE) ||
        (ln == LN_RANDOMSEED) || (ln == LN_USEFASTQUADS) ||
        (ln == LN_SCORESELECTION))
      return true;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    if (ln == LN_TIMEOUTLENGTH) {
      int32_t length = lv_integer(r, value);
      d->timeout_length = length > 0 ? (uint32_t)length : 0;
      d->last_input_tick = d->ticks;
      return true;
    }
    if (ln == LN_TIMEOUTSCRIPT) {
      snprintf(d->timeout_script, sizeof(d->timeout_script), "%s",
               lv_type(value) == LV_VOID ? "" : lv_cstr(r, value));
      for (char *c = d->timeout_script; *c; c++) *c = (char)tolower((unsigned char)*c);
      return true;
    }
#endif
  }
  if (lv_type(owner) == LV_VOID && (ln == LN_ACTORLIST)) {
    r->roots[DG_BEHAVIOR_ROOT + DG_SPRITES] = value;
    return true;
  }
  if (lv_type(owner) == LV_CASTLIB && (ln == LN_FILENAME)) {
    const dg_cast_t *lib = d10_castlib(d, lv_id(owner));
    if (!lib) {
      error(d, "cast library range", name);
      return true;
    }
    char stem[20];
    snprintf(stem, sizeof(stem), "%s", d10_path_base(lv_cstr(r, value)));
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    const dg_movie_t *target =
        d->platform.find_movie ? d->platform.find_movie(d->context, stem, 0)
                               : NULL;
    if (!target || target->id == lib->file) {
      // Pointing a cast at its own archive or at a plain file name selects
      // its mutable-record storage: the per-player profile mechanism.
      d10_cast_store_bind(d, lib, stem, true);
      return true;
    }
    unsigned slot = d->cast_swap_count;
    for (unsigned i = 0; i < d->cast_swap_count; i++)
      if (eq(d->cast_swaps[i].name, lib->name)) slot = i;
    if (slot == d->cast_swap_count) {
      if (slot >= sizeof(d->cast_swaps) / sizeof(d->cast_swaps[0])) {
        error(d, "cast swap capacity", lib->name);
        return true;
      }
      d->cast_swap_count++;
    }
    snprintf(d->cast_swaps[slot].name, sizeof(d->cast_swaps[slot].name), "%s", lib->name);
    snprintf(d->cast_swaps[slot].stem, sizeof(d->cast_swaps[slot].stem), "%s", stem);
    d->cast_swaps[slot].target = target;
    d10_window_share_one(d, target);
    // Sprites already showing the library's members show the new archive's.
    for (unsigned i = 1; i < DG_SPRITES; i++) {
      uint32_t id = d10_swap_member(d, d->sprites[i].value.member);
      if (id != d->sprites[i].value.member) {
        set_member(&d->sprites[i], id);
        dg_sprite_changed(d, i);
      }
    }
    if (d->platform.trace) {
      char text[96];
      snprintf(text, sizeof(text), "CAST_SWAP %s %s", lib->name, target->code->name);
      d->platform.trace(d->context, text);
    }
    return true;
  }
  if (imaging_set(d, name, owner, value)) return true;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Dynamic text members own their box dimensions. Font and foreground
  // changes continue through the shared field state setters below.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_WIDTH || ln == LN_HEIGHT)) {
    for (unsigned i = 0; i < d->dynamic_count; i++) {
      dg_member_t *m = &d->dynamic_members[i].member;
      if (m->type && m->id == (uint32_t)lv_id(owner)) {
        long size = lv_integer(r, value);
        if (size < 0 || size > 65535) {
          error(d, "invalid text dimensions", name);
          return true;
        }
        if (ln == LN_WIDTH) {
          m->width = (uint16_t)size;
          if (!m->height) m->height = 28;
        } else m->height = (uint16_t)size;
        d->visual_revision++;
        return true;
      }
    }
  }
  // the alertHook names a script that would answer alerts; the port's
  // alerts are traced, never shown.
  if (lv_type(owner) == LV_VOID && (ln == LN_ALERTHOOK)) return true;
  // A button's highlight is the platform's own pointer feedback.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_HILITE)) return true;
  // A text member's insertion point: entry runs through the platform's
  // own text box, so the selection is accepted as it stands.
  if (lv_type(owner) == LV_MEMBER && (ln == LN_SELECTION)) return true;
  // sprite.editable overrides the member's own setting.
  if (lv_type(owner) == LV_SPRITE && (ln == LN_EDITABLE) && lv_id(owner) >= 0 &&
      lv_id(owner) < DG_SPRITES) {
    d->sprite_editable[lv_id(owner)] = lv_integer(r, value) ? 1 : -1;
    return true;
  }
#endif
  // Public QuickTime sprite controls use ticks, including clip bounds.
  if (lv_type(owner) == LV_SPRITE && ((ln == LN_MOVIERATE) || (ln == LN_MOVIETIME) ||
      (ln == LN_STARTTIME) || (ln == LN_STOPTIME) || (ln == LN_VOLUME))) {
    dg_sprite_t *s = sprite(d, owner);
    const dg_member_t *m = s && s->value.type ? dg_member(d, s->value.member) : NULL;
    if (dg_is_video(m)) {
      d10_video_init(d, s);
      double number = lv_number(r, value);
      if (!isfinite(number)) { error(d, "invalid video control", name); return true; }
      if (ln == LN_MOVIERATE) s->video_rate = number;
      else if (ln == LN_VOLUME) s->video_volume = (unsigned)fmin(255, fmax(0, number));
      else if (ln == LN_STOPTIME) s->video_stop = (unsigned)fmin(m->samples, fmax(0, number * 10));
      else {
        d10_video_seek(s, m, number * 10);
        if (ln == LN_STARTTIME) s->video_start = (unsigned)s->video_time;
      }
      s->video_serial++;
      dg_sprite_changed(d, (unsigned)lv_id(owner));
    }
    return true;
  }
  if (lv_type(owner) == LV_TIMEOUT) {
    unsigned slot = (unsigned)lv_id(owner) - 1;
    if (slot >= DG_TIMEOUTS) return false;
    if (ln == LN_PERSISTENT) {
      d->timeouts[slot].persistent = lv_integer(r, value) != 0;
      return true;
    }
    if (ln == LN_PERIOD) {
      uint32_t ms = (uint32_t)lv_integer(r, value);
      d->timeouts[slot].period = ms;
      d->timeouts[slot].next = d->ticks + (ms * 60u + 999u) / 1000u;
      return true;
    }
    // Reschedule to fire at an absolute milliSeconds value.
    if (ln == LN_TIME) {
      int64_t delay = (int64_t)lv_integer(r, value) - (int64_t)(d->clock / 60000u);
      d->timeouts[slot].next =
          d->ticks + (delay > 0 ? (uint32_t)((delay * 60 + 999) / 1000) : 0u);
      return true;
    }
    if (ln == LN_TARGET) {
      r->roots[DG_TIMEOUT_ROOT + slot] = value;
      return true;
    }
  }
  if (lv_type(owner) != LV_WINDOW) return false;
  if (ln == LN_FILENAME) { d10_window_request(d, lv_cstr(r, value)); return true; }
  if (ln == LN_VISIBLE) {
    d->window_visible = lv_integer(r, value) != 0;
    if (d->window_visible && d->platform.trace)
      d->platform.trace(d->context, "WINDOW_PANEL_PENDING_RENDER");
    return true;
  }
  if ((ln == LN_RECT) || (ln == LN_DRAWRECT)) {
    for (unsigned i = 0; i < 4; i++)
      d->window_rect[i] = (int)lv_integer(r, lv_at(r, value, i + 1));
    return true;
  }
  if ((ln == LN_TITLE) || (ln == LN_TITLEVISIBLE) || (ln == LN_WINDOWTYPE))
    return true;
  return false;
}
static bool d10_call(dg_runtime_t *d, const char *name, unsigned argc,
                     const lv_t *a, lv_t *out) {
  if(d10_avm_call(d,name,argc,a,out))return true;
  if (linked_call(d, name, argc, a, out)) return true;
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
  // The converter proves these small AVM1 functions from their instructions,
  // independent of function names. Keep their call semantics after rasterizing
  // the timeline (for example a helper that adds its argument to a child's y).
  unsigned receiver_slot = argc ? d10_flash_slot(d, a[0]) : 0;
  unsigned helper_channel = receiver_slot ? d->flash_objects[receiver_slot - 1].sprite
      : argc && lv_type(a[0]) == LV_SPRITE ? (unsigned)lv_id(a[0]) : 0;
  if (helper_channel > 0 && helper_channel < DG_SPRITES) {
    const dg_member_t *m = dg_member(d, d->sprites[helper_channel].value.member);
    for (unsigned i = 0; m && i < m->flash_helper_count; i++) {
      const dg_flash_helper_t *helper = &m->flash_helpers[i];
      if (!eq(name, helper->name) || helper->method != (receiver_slot != 0)) continue;
      if (receiver_slot && !eq(helper->target, d->flash_objects[receiver_slot - 1].name)) continue;
      unsigned arg = helper->argument + 1u;
      if (arg >= argc || (helper->operation != 4 && lv_type(a[arg]) != LV_NUMBER)) {
        error(d, "invalid Flash helper argument", name);
        return true;
      }
      lv_t object = receiver_slot ? a[0] : (lv_t){0};
      if (!receiver_slot) d10_flash_get(d, helper->target, a[0], &object);
      if (lv_type(object) != LV_PROPLIST) {
        error(d, "Flash helper target is unavailable", helper->target);
        return true;
      }
      const dg_flash_field_t *field = dg_flash_field_find(m, helper->target);
      if (helper->operation == 3 || helper->operation == 4) {
        if (!field) {
          error(d, "Flash text helper requires a text field", helper->target);
          return true;
        }
        if (helper->operation == 3) {
          double size = lv_numeric(a[arg]);
          if (!isfinite(size) || size <= 0 || size > 65535) {
            error(d, "Flash text size is outside rendering bounds", name);
            return true;
          }
          lv_set(r, NULL, "size", object, lv_num(size));
        } else {
          const char *encoded = lv_cstr(r, a[arg]);
          size_t capacity = strlen(encoded) + 1;
          char *decoded = malloc(capacity);
          if (!decoded) {
            error(d, "Flash text allocation failed", name);
            return true;
          }
          d10_flash_unescape(encoded, decoded, capacity);
          lv_t text = lv_text(r, decoded, false);
          free(decoded);
          lv_set(r, NULL, "text", object, text);
        }
        const char *text = lv_cstr(r, lv_get(r, NULL, "text", object));
        unsigned height = d10_flash_text_height(d, field, object, text);
        lv_set(r, NULL, "textheight", object, lv_num(height));
      } else {
        double value = lv_numeric(a[arg]);
        if (helper->operation == 1) {
          lv_t old = lv_get(r, NULL, helper->property, object);
          if (lv_type(old) != LV_NUMBER) {
            error(d, "Flash helper requires a numeric property", helper->property);
            return true;
          }
          value += lv_numeric(old);
        } else if (helper->operation != 2) {
          error(d, "unsupported Flash helper operation", name);
          return true;
        }
        lv_set(r, NULL, helper->property, object, lv_num(value));
      }
      *out = (lv_t){0};
      return true;
    }
  }
  if ((ln == LN_WINDOW) && argc == 1) {
    if (!d10_stem_matches("CONTROL", lv_cstr(r, a[0]))) {
      script_error(d, "unsupported movie window", lv_cstr(r, a[0]));
      return true;
    }
    *out = lv_make(LV_WINDOW, 1);
    return true;
  }
  if ((ln == LN_TELL_WINDOW) && argc == 1) { d->tell_window = true; return true; }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // vList: its registration is a licence key; a file object's write() keeps
  // a value as its Lingo literal and read() answers it, VOID when absent.
  if (ln == LN_VLIST_REGISTER) { *out = lv_num(1); return true; }
  if ((ln == LN_SYSTEMDATE) && !argc) { *out = d85_system_date(r); return true; }
  // date(year, month, day) as its day number, so dates subtract to days
  // (the Adventskalender counts down to December).
  if ((ln == LN_DATE) && argc == 3) {
    int64_t y = lv_integer(r, a[0]), m = lv_integer(r, a[1]), day = lv_integer(r, a[2]);
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    *out = lv_num((double)(era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468));
    return true;
  }
  if (((ln == LN_READ) && argc == 1) || ((ln == LN_WRITE) && argc == 2)) {
    lv_t file = lv_type(a[0]) == LV_PROPLIST ? lv_get(r, NULL, "vlistfile", a[0]) : (lv_t){0};
    if (lv_type(file) == LV_STRING) {
      static char text[32768];
      unsigned length = 0;
      if (ln == LN_WRITE) {
        bool ok = lv_format(r, a[1], text, sizeof(text)) && d->platform.write_file &&
                  d->platform.write_file(d->context, lv_cstr(r, file), text, (unsigned)strlen(text));
        *out = lv_num(ok);
      } else if (d->platform.read_file &&
                 d->platform.read_file(d->context, lv_cstr(r, file), text, sizeof(text) - 1, &length) && length) {
        text[length] = 0;
        *out = lv_literal(r, text);
      } else {
        *out = (lv_t){0};
      }
      return true;
    }
  }
#endif
  // The PropSave Xtra: a value saved as its Lingo literal and read back as
  // value() would, through the game's save files by name.
  if ((ln == LN_SAVEPROPSTODISK) && argc == 2) {
    static char text[32768];
    const char *file = d10_path_base(lv_cstr(r, a[0]));
    bool ok = lv_format(r, a[1], text, sizeof(text)) && d->platform.write_file &&
              d->platform.write_file(d->context, file, text, (unsigned)strlen(text));
    // PropSave answers VOID when it saved and an error number when not.
    *out = ok ? (lv_t){0} : lv_num(-1);
    return true;
  }
  if ((ln == LN_LOADPROPSFROMDISK) && argc == 1) {
    static char text[32768];
    unsigned length = 0;
    const char *file = d10_path_base(lv_cstr(r, a[0]));
    if (!d->platform.read_file || !d->platform.read_file(d->context, file, text, sizeof(text) - 1, &length) ||
        !length) {
      // An error number, as PropSave answers for a missing file.
      *out = lv_num(-1);
      return true;
    }
    text[length] = 0;
    *out = lv_literal(r, text);
    return true;
  }
  if ((ln == LN_PARAM) && argc == 1) { *out = lv_param(r, (unsigned)lv_integer(r, a[0])); return true; }
  // The rest of the sprite's behaviors skip the current event.
  if ((ln == LN_STOPEVENT) && !argc) { d->behavior_channel = d->behavior_cursor = 0; return true; }
  // An Xtra's serial-number registration (PrintOMatic's register).
  if (ln == LN_REGISTER) { *out = lv_num(0); return true; }
  if ((ln == LN_GETOSDIRECTORY) && !argc) { *out = lv_text(r, "C:\\WINDOWS", false); return true; }
  if ((ln == LN_QUICKTIMEVERSION) && !argc) { *out = lv_num(6); return true; }
  // A projector halts by quitting.
  if ((ln == LN_HALT) && !argc) { d->quit = true; return true; }
  // Custom menu bars: the target shows none.
  if (ln == LN_INSTALLMENU) return true;
  if ((ln == LN_MENU) && argc == 1) { *out = (lv_t){0}; return true; }
  // A drive holding a file: the target has none to search.
  if ((ln == LN_BAFINDDRIVE) && argc == 2) { *out = lv_text(r, "", false); return true; }
  // Screen geometry for the system tests: the 800x600 true-colour stage.
  if ((ln == LN_BASCREENINFO) && argc == 1) {
    const char *what = lv_cstr(r, a[0]);
    *out = lv_num(d10_ends_ci(what, "width") ? 800 : d10_ends_ci(what, "height") ? 600
                  : d10_ends_ci(what, "depth") ? 32 : 0);
    return true;
  }
  // Windows-only Xtra registrations succeed as no-ops on the target; a
  // missing handler would abort the caller under alert-and-continue.
  if (ln == LN_BAREGISTER) {
    *out = lv_num(1);
    return true;
  }
  // Constructing an absent optional Xtra stays a non-object.
  if ((ln == LN_NEW) && argc == 1 && lv_type(a[0]) == LV_VOID) {
    *out = (lv_t){0};
    return true;
  }
  // Authored timeout objects: timeout("name") answers a stable handle;
  // .new(period_ms, #handler, target) schedules the rooted target's handler
  // on the service clock and .forget() releases the slot.
  if ((ln == LN_TIMEOUT) && argc == 1 &&
      (lv_type(a[0]) == LV_STRING || lv_type(a[0]) == LV_SYMBOL)) {
    const char *timeout_name = lv_cstr(r, a[0]);
    int slot = -1;
    for (unsigned i = 0; i < DG_TIMEOUTS && slot < 0; i++)
      if (d->timeouts[i].active && eq(d->timeouts[i].name, timeout_name))
        slot = (int)i;
    for (unsigned i = 0; i < DG_TIMEOUTS && slot < 0; i++)
      if (!d->timeouts[i].active) slot = (int)i;
    if (slot < 0) {
      error(d, "timeout capacity", timeout_name);
      return true;
    }
    snprintf(d->timeouts[slot].name, sizeof(d->timeouts[slot].name), "%s",
             timeout_name);
    *out = lv_make(LV_TIMEOUT, slot + 1);
    return true;
  }
  // Without a target object the handler is a movie handler (Optik's quiz
  // answer timer).
  if ((ln == LN_NEW) && (argc == 4 || argc == 3) && lv_type(a[0]) == LV_TIMEOUT) {
    unsigned slot = (unsigned)lv_id(a[0]) - 1;
    if (slot >= DG_TIMEOUTS) return false;
    uint32_t ms = (uint32_t)lv_integer(r, a[1]);
    snprintf(d->timeouts[slot].handler, sizeof(d->timeouts[slot].handler),
             "%s", lv_cstr(r, a[2]));
    d->timeouts[slot].period = ms;
    d->timeouts[slot].next = d->ticks + (ms * 60u + 999u) / 1000u;
    d->timeouts[slot].active = true;
    d->timeouts[slot].persistent = false;
    r->roots[DG_TIMEOUT_ROOT + slot] = argc == 4 ? a[3] : (lv_t){0};
    *out = a[0];
    return true;
  }
  if ((ln == LN_FORGET) && argc == 1 && lv_type(a[0]) == LV_TIMEOUT) {
    unsigned slot = (unsigned)lv_id(a[0]) - 1;
    if (slot < DG_TIMEOUTS) {
      d->timeouts[slot].active = false;
      r->roots[DG_TIMEOUT_ROOT + slot] = (lv_t){0};
    }
    return true;
  }
  // castLib("save").save(path) creates a save file from the current
  // records: rebind the storage, then the shared save arm writes it out.
  if ((ln == LN_SAVE) && argc == 2 && lv_type(a[0]) == LV_CASTLIB) {
    const dg_cast_t *lib = d10_castlib(d, lv_id(a[0]));
    if (!lib) {
      error(d, "new cast library", "invalid");
      return true;
    }
    char stem[24];
    snprintf(stem, sizeof(stem), "%s", d10_path_base(lv_cstr(r, a[1])));
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    d10_cast_store_bind(d, lib, stem, false);
    bool ok = d6_save_cast(d, lib);
    *out = lv_num(ok);
    if (!ok) error(d, "cast save", "failed");
    return true;
  }
  // label("name") answers the labelled frame, zero when absent — the
  // authored existence probe before a go().
  if ((ln == LN_LABEL) && argc == 1 &&
      (lv_type(a[0]) == LV_STRING || lv_type(a[0]) == LV_SYMBOL)) {
    const char *label_name = lv_cstr(r, a[0]);
    unsigned frame_number = 0;
    for (unsigned i = 0; i < d->movie->label_count; i++)
      if (eq(d->movie->labels[i].name, label_name))
        frame_number = d->movie->labels[i].frame;
    *out = lv_num(frame_number);
    return true;
  }
  // Memory preloading is a no-op: package assets stream on demand.
  if ((ln == LN_PRELOAD) || (ln == LN_PRELOADMEMBER) ||
      (ln == LN_PRELOADCAST) || (ln == LN_PRELOADNETTHING)) {
    *out = lv_num(1);
    return true;
  }
  if ((ln == LN_FLUSHINPUTEVENTS) && argc == 0) {
    // Drops queued-but-undispatched key events, exactly the builtin's scope.
    d->key_count = 0;
    d->key_first = 0;
    return true;
  }
  if ((ln == LN_WINDOWPRESENT) && argc == 1) {
    *out = lv_num(d->window_movie_name[0] && d10_stem_matches("CONTROL", lv_cstr(r, a[0])));
    return true;
  }
  if ((ln == LN_TELL_END) || (ln == LN_TELL_STAGE)) {
    d->tell_window = false;
    return true;
  }
  // Film channels have the profile's complete channel range. Nested film
  // tells require another instance context, rather than aliasing the stage.
  if ((ln == LN_TELL_CHILD) && argc == 2) {
    lv_t parent=lv_type(a[0])==LV_SPRITE?a[0]:lv_make(LV_SPRITE,(int32_t)lv_integer(r,a[0]));
    *out=dg_film_tell(d,parent,(unsigned)lv_integer(r,a[1]));
    return true;
  }
  // Sprite tells drive a flattened film timeline: one-based frames over the
  // sprite's film assets, holding past the end exactly as film advancement.
  if (((ln == LN_SPRITE_FRAME) || (ln == LN_SPRITE_LASTFRAME)) && argc == 1) {
    unsigned native_frame=dg_film_frame(d,a[0],ln==LN_SPRITE_LASTFRAME);
    if(native_frame){*out=lv_num(native_frame);return true;}
    unsigned n = lv_type(a[0]) == LV_SPRITE ? (unsigned)lv_id(a[0]) : (unsigned)lv_integer(r, a[0]);
    const dg_member_t *m = n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
    if (!m || !m->film_count) { *out = lv_num(0); return true; }
    unsigned pose = d->sprites[n].film_frame < m->film_count
                        ? d->sprites[n].film_frame
                        : (unsigned)m->film_count - 1;
    *out = lv_num(name[7] == 'l' ? (unsigned)m->film_count : pose + 1);
    return true;
  }
  if ((ln == LN_SPRITE_GO) && argc == 2) {
    unsigned n = lv_type(a[0]) == LV_SPRITE ? (unsigned)lv_id(a[0]) : (unsigned)lv_integer(r, a[0]);
    long frame = 0;
    if (lv_type(a[1]) != LV_NUMBER) {
      // Flash frame labels ("red", "sel") name positions inside the authored
      // timeline; the recorded label table maps them onto the flattened film
      // poses. An unknown label holds the current pose, traced.
      const char *label = lv_cstr(r, a[1]);
      const dg_member_t *m =
          n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
      for (unsigned i = 0; m && i < m->flash_label_count; i++)
        if (eq(m->flash_labels[i].name, label))
          frame = m->flash_labels[i].frame;
      if (!frame) {
        if (d->platform.trace) {
          char text[96];
          snprintf(text, sizeof(text), "FLASH_LABEL_PENDING sprite=%u label=%s",
                   n, label);
          d->platform.trace(d->context, text);
        }
        return true;
      }
    } else {
      frame = (long)lv_integer(r, a[1]);
    }
    if(frame>=1 && dg_film_go(d,a[0],(unsigned)frame))return true;
    if (n < DG_SPRITES && frame >= 1 &&
        d->sprites[n].film_frame != (unsigned)frame - 1) {
      d->sprites[n].film_frame = (unsigned)frame - 1;
      dg_sprite_changed(d, n);
    }
    if (n < DG_SPRITES && frame >= 1)
      film_stop_action(dg_member(d, d->sprites[n].value.member), &d->sprites[n]);
    if (n < DG_SPRITES && frame >= 1)
      flash_seek(d,&d->sprites[n],dg_member(d,d->sprites[n].value.member));
    return true;
  }
  // findLabel answers the authored frame number of a Flash timeline label,
  // or zero for none — the authored fallback then seeks frame one.
  if ((ln == LN_FINDLABEL) && argc == 2 && lv_type(a[0]) == LV_SPRITE) {
    unsigned n = (unsigned)lv_id(a[0]);
    const dg_member_t *m =
        n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
    const char *label = lv_cstr(r, a[1]);
    unsigned frame = 0;
    for (unsigned i = 0; m && i < m->flash_label_count; i++)
      if (eq(m->flash_labels[i].name, label))
        frame = m->flash_labels[i].frame;
    *out = lv_num(frame);
    return true;
  }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // The sound object's playlist: queue and setPlaylist line sounds up,
  // play() starts the first and the channel then works through the rest;
  // play(sound) starts that one at once and the queue follows it.
  if (((ln == LN_QUEUE) || (ln == LN_SETPLAYLIST) || (ln == LN_GETPLAYLIST) ||
       (ln == LN_PLAY)) &&
      argc >= 1 && lv_type(a[0]) == LV_SOUND) {
    unsigned ch = (unsigned)lv_id(a[0]) - 1;
    if (ch >= DG_SOUND_CHANNELS) {
      error(d, "audio channel", name);
      return true;
    }
    if ((ln == LN_QUEUE) && argc == 2) {
      d85_sound_queue(d, ch, d85_sound_entry(d, a[1]));
      return true;
    }
    if ((ln == LN_SETPLAYLIST) && argc == 2) {
      d->sound_queue_count[ch] = 0;
      unsigned count = lv_type(a[1]) == LV_LIST ? lv_count(r, a[1]) : 0;
      for (unsigned i = 1; i <= count; i++) d85_sound_queue(d, ch, d85_sound_entry(d, lv_at(r, a[1], i)));
      return true;
    }
    if ((ln == LN_GETPLAYLIST) && argc == 1) {
      lv_t items[DG_SOUND_QUEUE];
      for (unsigned i = 0; i < d->sound_queue_count[ch]; i++) {
        lv_t pair[2] = {lv_symbol(r, "member"), lv_make(LV_MEMBER, (int32_t)d->sound_queue[ch][i])};
        items[i] = lv_list(r, 2, pair, true);
      }
      *out = lv_list(r, d->sound_queue_count[ch], items, false);
      return true;
    }
    if ((ln == LN_PLAY) && argc == 1) {
      if (d->sound_paused[ch]) {
        if (!d->platform.sound_pause) error(d, "sound pause service unavailable", name);
        else { d->platform.sound_pause(d->context, ch, false); d->sound_paused[ch] = false; }
      }
      if (!d85_sound_busy(d, ch) && d->sound_queue_count[ch]) d85_sound_next(d, ch);
      d->sound_chain[ch] = true;
      return true;
    }
    if ((ln == LN_PLAY) && argc == 2 && lv_type(a[1]) == LV_PROPLIST) {
      d85_sound_start(d, ch, d85_sound_entry(d, a[1]));
      d->sound_chain[ch] = true;
      return true;
    }
    if ((ln == LN_PLAY) && argc == 2) d->sound_chain[ch] = true;
  }
#endif
  // sound(n).play(member) starts a cast sound on the channel; the score's
  // puppet flag holds so frame sounds do not steal the channel.
  if ((ln == LN_PLAY) && argc == 2 && lv_type(a[0]) == LV_SOUND &&
      lv_type(a[1]) == LV_MEMBER) {
    unsigned play_channel = (unsigned)lv_id(a[0]) - 1;
    if (play_channel >= DG_SOUND_CHANNELS) {
      error(d, "audio channel", name);
      return true;
    }
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(a[1]));
    if (m && m->type == 6) {
      d->sound_puppet[play_channel] = true;
      d->sound_serial[play_channel]++;
      play_member(d, play_channel, m);
    } else if (d->platform.trace) {
      d->platform.trace(d->context, "SOUND_PLAY_EMPTY");
    }
    return true;
  }
  // getaProp/setaProp on a sprite address its behavior properties by
  // dynamic name; an absent property answers VOID, never an alert.
  if (((ln == LN_SPRITE_GETAPROP) || (ln == LN_SPRITE_SETAPROP)) &&
      argc >= 2 && lv_type(a[0]) == LV_SPRITE) {
    const char *prop = lv_cstr(r, a[1]);
    lv_t instance = behavior_property(d, (unsigned)lv_id(a[0]), prop);
    if (name[7] == 'g') {
      *out = lv_type(instance) == LV_INSTANCE ? lv_get(r, NULL, prop, instance)
                                          : (lv_t){0};
    } else if (argc >= 3 && lv_type(instance) == LV_INSTANCE) {
      lv_set(r, NULL, prop, instance, a[2]);
    }
    return true;
  }
  // Speech queues poll their channel between streamed files.
  if ((ln == LN_ISBUSY) && argc == 1 && lv_type(a[0]) == LV_SOUND) {
    *out = lv_num(lv_id(a[0]) >= 1 && lv_id(a[0]) <= DG_SOUND_CHANNELS &&
                  d->platform.sound_busy &&
                  d->platform.sound_busy(d->context, (unsigned)lv_id(a[0]) - 1));
    return true;
  }
  // DirectMedia's sprite functions: videoSeek takes milliseconds.
  if (((ln == LN_VIDEOPAUSE) || (ln == LN_VIDEOPLAY) || (ln == LN_VIDEOSEEK) ||
       (ln == LN_VIDEOSTOP)) && argc >= 1 && lv_type(a[0]) == LV_SPRITE) {
    dg_sprite_t *s = sprite(d, a[0]);
    const dg_member_t *m = s && s->value.type ? dg_member(d, s->value.member) : NULL;
    if (!dg_is_video(m))
      return true;
    d10_video_init(d, s);
    if (ln == LN_VIDEOPLAY) {
      if (s->video_time >= m->samples)
        d10_video_seek(s, m, 0);
      s->video_rate = 1;
    } else if (ln == LN_VIDEOPAUSE) {
      s->video_rate = 0;
    } else if (ln == LN_VIDEOSTOP) {
      s->video_rate = 0;
      d10_video_seek(s, m, 0);
    } else if (argc >= 2) {
      d10_video_seek(s, m, lv_number(r, a[1]) * 6 / 10);
    }
    s->video_serial++;
    dg_sprite_changed(d, (unsigned)lv_id(a[0]));
    return true;
  }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // ticks() and milliseconds() as functions answer as `the` properties do.
  if (((ln == LN_TICKS) || (ln == LN_MILLISECONDS)) && argc == 0) {
    *out = lv_num((ln == LN_TICKS) ? (double)d->ticks : (double)(d->clock / 60000u));
    return true;
  }
  // xmlParser.parseString(text): the document element becomes the parser's
  // one child; getError() answers VOID once that worked.
  if ((ln == LN_PARSESTRING) && argc == 2 && lv_type(a[0]) == LV_PROPLIST) {
    const char *text = lv_cstr(r, a[1]);
    d85_xml_t x = {text, text + strlen(text), NULL};
    for (;;) {
      d85_xml_space(&x);
      if (d85_xml_starts(&x, "<?")) d85_xml_skip_to(&x, "?>");
      else if (d85_xml_starts(&x, "<!")) d85_xml_skip_to(&x, ">");
      else break;
    }
    lv_t root = d85_xml_element(r, &x, 0);
    if (x.error) {
      lv_set(r, NULL, "xmlerror", a[0], lv_text(r, x.error, false));
      *out = lv_num(-1);
      return true;
    }
    lv_set(r, NULL, "child", a[0], lv_list(r, 1, &root, false));
    lv_set(r, NULL, "xmlerror", a[0], (lv_t){0});
    *out = lv_num(0);
    return true;
  }
  if ((ln == LN_GETERROR) && argc == 1 && lv_type(a[0]) == LV_PROPLIST) {
    *out = lv_get(r, NULL, "xmlerror", a[0]);
    return true;
  }
#endif
  // script.handlers() and instance.handlers(): the script's own handler
  // names as symbols (Circus's event broker checks for receive handlers).
  if ((ln == LN_HANDLERS) && argc == 1 &&
      (lv_type(a[0]) == LV_INSTANCE || lv_type(a[0]) == LV_SCRIPT)) {
    lv_t script = lv_type(a[0]) == LV_SCRIPT ? a[0] : lv_get(r, NULL, "script", a[0]);
    lv_t names[128];
    unsigned count = 0;
    const dg_movie_t *movie =
        lv_type(script) == LV_SCRIPT ? dg_loaded(d, DG_ID_FILE(lv_id(script))) : NULL;
    unsigned cast = DG_ID_CAST(lv_id(script));
    if (movie && cast && cast <= movie->cast_count) {
      const char *cast_name = movie->casts[cast - 1].name;
      unsigned member = (uint32_t)lv_id(script) & 65535;
      for (unsigned i = 0; i < movie->code->count && count < 128; i++) {
        const lv_handler_t *h = &movie->code->handlers[i];
        if (h->member == member && eq(h->cast, cast_name))
          names[count++] = lv_symbol(r, h->name);
      }
    }
    *out = lv_list(r, count, names, false);
    return true;
  }
  // A paused channel retains its sample position and remains busy.
  if (((ln == LN_PAUSE) || (ln == LN_PLAY)) && argc == 1 &&
      lv_type(a[0]) == LV_SOUND) {
    unsigned channel = (unsigned)lv_id(a[0]);
    if (!channel || channel > DG_SOUND_CHANNELS)
      error(d, "sound channel", name);
    else if (!d->platform.sound_pause)
      error(d, "sound pause service unavailable", name);
    else {
      d->platform.sound_pause(d->context, channel - 1, ln == LN_PAUSE);
      d->sound_paused[channel - 1] = ln == LN_PAUSE;
    }
    return true;
  }
  if (imaging_call(d, name, argc, a, out)) return true;
  // Flash playback holds the current pose until explicitly resumed.
  if ((ln == LN_GOTOFRAME) && argc == 2 && lv_type(a[0]) == LV_SPRITE)
    return d10_call(d, "sprite_go", 2, a, out);
  if ((ln == LN_STOP) && argc == 1 && lv_type(a[0]) == LV_SPRITE) {
    unsigned n = (unsigned)lv_id(a[0]);
    if (n < DG_SPRITES) {
      d->sprites[n].film_paused = true;
      dg_flash_play(d,n,false);
    }
    return true;
  }
  if ((ln == LN_SOUND_PLAYFILE) && argc == 2) {
    long channel = lv_integer(r, a[0]);
    if (channel < 1 || channel > DG_SOUND_CHANNELS) {
      error(d, "sound channel", name);
      return true;
    }
    // The authored path ends "...\voc\NAME.aif"; the converted stream is the
    // virtual "folder/stem" wav64. Never interpret the path as a host path.
    const char *path = lv_cstr(r, a[1]);
    const char *base = path, *folder = path;
    for (const char *p = path; *p; p++)
      if (*p == '\\' || *p == '/' || *p == ':') { folder = base; base = p + 1; }
    char virtual_name[96];
    unsigned folder_length = 0;
    while (folder[folder_length] && folder[folder_length] != '\\' &&
           folder[folder_length] != '/' && folder[folder_length] != ':')
      folder_length++;
    snprintf(virtual_name, sizeof(virtual_name), "%.*s/%s",
             (int)(folder == base ? 0 : folder_length), folder, base);
    char *extension = strrchr(virtual_name, '.');
    if (extension) *extension = 0;
    // Windows resolved the authored path case-insensitively; the converted
    // stream set canonicalizes to lowercase (sfx\silence.aif names the
    // on-disk Silence.aif).
    for (char *p = virtual_name; *p; p++)
      if (*p >= 'A' && *p <= 'Z') *p += 32;
    d->sound_paused[(unsigned)channel - 1] = false;
    if (d->platform.play_file) {
      sound_host(d)->linked_sound_owner[(unsigned)channel - 1] = d;
      d->sound_puppet[(unsigned)channel - 1] = true;
      d->platform.play_file(d->context, (unsigned)channel - 1, virtual_name);
    } else if (d->platform.trace) {
      char text[128];
      snprintf(text, sizeof(text), "PLAY_FILE channel=%ld name=%s", channel, virtual_name);
      d->platform.trace(d->context, text);
    }
    return true;
  }
  // Buddy API surface. The projector used it for preference INI files (the
  // save archive serves those), display management (the target renders
  // one fixed 800x600x32 mode, so switching code observes a satisfied
  // display) and shell dialogs (which answer as dismissed).
  // ActionScript methods on a Flash sprite's text field. setText decodes the
  // authored percent-escapes and stores the UTF-8 text the overlay renders;
  // the laid-out textHeight follows so the authored vertical centering runs.
  if (argc >= 1 && d10_flash_slot(d, a[0]) &&
      ((ln == LN_SETTEXT) || (ln == LN_SETTEXTFORMAT) ||
       (ln == LN_GETTEXTFORMAT) ||
       (ln == LN_GOTOANDSTOP) || (ln == LN_GOTOANDPLAY) ||
       (ln == LN_PLAY) || (ln == LN_STOP))) {
    unsigned slot = d10_flash_slot(d, a[0]) - 1;
    unsigned sprite = d->flash_objects[slot].sprite;
    const dg_member_t *m = dg_member(d, d->sprites[sprite].value.member);
    const dg_flash_field_t *field =
        dg_flash_field_find(m, d->flash_objects[slot].name);
    if ((ln == LN_SETTEXT) && argc >= 2) {
      static char decoded[2048];
      d10_flash_unescape(lv_cstr(r, a[1]), decoded, sizeof(decoded));
      lv_set(r, NULL, "text", a[0], lv_text(r, decoded, false));
      if (field)
        lv_set(r, NULL, "textheight", a[0],
               lv_num(d10_flash_text_height(d, field, a[0], decoded)));
      dg_sprite_changed(d, sprite);
    }
    if (ln == LN_GETTEXTFORMAT) {
      // The whole-field format; a ranged request answers the same values
      // and the ranged apply below records itself as an approximation.
      lv_t pairs[] = {
          lv_text(r, "size", true), lv_get(r, NULL, "size", a[0]),
          lv_text(r, "color", true), lv_get(r, NULL, "color", a[0]),
      };
      *out = lv_list(r, sizeof(pairs) / sizeof(*pairs), pairs, true);
    }
    if (ln == LN_SETTEXTFORMAT) {
      lv_t format = argc >= 4 ? a[3] : argc >= 2 ? a[1] : (lv_t){0};
      if (argc >= 4) {
        // A character-range format: the overlay renders the whole prompt in
        // its base style, recorded and traced as an approximation.
        if (d->platform.trace)
          d->platform.trace(d->context, "FLASH_FORMAT_RANGE_PENDING");
      } else if (lv_type(format) == LV_PROPLIST) {
        lv_t size = lv_get(r, NULL, "size", format);
        if (lv_type(size) == LV_NUMBER && lv_numeric(size) > 0)
          lv_set(r, NULL, "size", a[0], size);
        lv_t color = lv_get(r, NULL, "color", format);
        if (lv_type(color) != LV_VOID)
          lv_set(r, NULL, "color", a[0], color);
        if (field) {
          const char *text = lv_cstr(r, lv_get(r, NULL, "text", a[0]));
          lv_set(r, NULL, "textheight", a[0],
                 lv_num(d10_flash_text_height(d, field, a[0], text)));
        }
        dg_sprite_changed(d, sprite);
      }
    }
    return true;
  }
  // setVariable writes a Flash field through its bound variable name; the
  // one authored site targets the prompt field ("text1").
  if (((ln == LN_SETVARIABLE) || (ln == LN_GETVARIABLE)) &&
      argc >= 2 && lv_type(a[0]) == LV_SPRITE && lv_id(a[0]) >= 1 &&
      lv_id(a[0]) < DG_SPRITES) {
    const dg_member_t *m = dg_member(d, d->sprites[lv_id(a[0])].value.member);
    const char *variable = lv_cstr(r, a[1]);
    const dg_flash_field_t *field = NULL;
    for (unsigned i = 0; m && i < m->flash_field_count; i++)
      if (eq(m->flash_fields[i].variable, variable))
        field = &m->flash_fields[i];
    if (!field) {
      if (d->platform.trace) {
        char text[96];
        snprintf(text, sizeof(text), "FLASH_VARIABLE_PENDING sprite=%d name=%s",
                 (int)lv_id(a[0]), variable);
        d->platform.trace(d->context, text);
      }
      *out = (lv_t){0};
      return true;
    }
    lv_t object = {0};
    d10_flash_get(d, field->name, a[0], &object);
    if (lv_type(object) != LV_PROPLIST)
      return true;
    if (name[0] == 's' && argc >= 3) {
      static char decoded[2048];
      d10_flash_unescape(lv_cstr(r, a[2]), decoded, sizeof(decoded));
      lv_set(r, NULL, "text", object, lv_text(r, decoded, false));
      lv_set(r, NULL, "textheight", object,
             lv_num(d10_flash_text_height(d, field, object, decoded)));
      dg_sprite_changed(d, (unsigned)lv_id(a[0]));
    } else if (name[0] == 'g') {
      *out = lv_get(r, NULL, "text", object);
    }
    return true;
  }
  // Director's property symbols differ from ActionScript names: #posX and
  // #posY address the child's registration point, not `_posx`/`_posy`.
  if (((ln == LN_SETFLASHPROPERTY) || (ln == LN_GETFLASHPROPERTY)) &&
      argc >= 3 && lv_type(a[0]) == LV_SPRITE) {
    char key[32];
    const char *property = lv_cstr(r, a[2]);
    snprintf(key, sizeof(key), "%s%s", *property == '_' ? "" : "_", property);
    for (char *p = key; *p; p++)
      if (*p >= 'A' && *p <= 'Z') *p += 32;
    if (eq(key, "_posx")) strcpy(key, "_x");
    else if (eq(key, "_posy")) strcpy(key, "_y");
    lv_t object = {0};
    d10_flash_get(d, lv_cstr(r, a[1]), a[0], &object);
    if (lv_type(object) != LV_PROPLIST) {
      *out = (lv_t){0};
      return true;
    }
    if (name[0] == 's' && argc >= 4) {
      lv_set(r, NULL, key, object, a[3]);
      if (lv_id(a[0]) >= 1 && lv_id(a[0]) < DG_SPRITES)
        dg_sprite_changed(d, (unsigned)lv_id(a[0]));
    } else {
      *out = lv_get(r, NULL, key, object);
    }
    return true;
  }
  // The exercise task databases are read as a list of byte values and then
  // parsed by authored Lingo. An absent file answers VOID, as the Xtra does.
  if ((ln == LN_BAREADBINFILE) && argc >= 1) {
    char virtual_path[96];
    unsigned length = 0;
    *out = (lv_t){0};
    if (!d10_data_path(lv_cstr(r, a[0]), virtual_path, sizeof(virtual_path)) ||
        !d->platform.read_data ||
        !d->platform.read_data(d->context, virtual_path, d10_ini_data,
                               sizeof(d10_ini_data), &length)) {
      if (d->platform.trace) {
        char text[160];
        snprintf(text, sizeof(text), "DATA_FILE_MISSING %s", lv_cstr(r, a[0]));
        d->platform.trace(d->context, text);
      }
      return true;
    }
    *out = lv_byte_list(r, (const unsigned char *)d10_ini_data, length);
    return true;
  }
  if ((ln == LN_BAREADINI) && argc == 4) {
    const char *file = d10_path_base(lv_cstr(r, a[3]));
    unsigned length = 0;
    d10_ini_hit_t hit = {0};
    if (d->platform.read_file &&
        d->platform.read_file(d->context, file, d10_ini_data,
                              sizeof(d10_ini_data), &length) &&
        length <= sizeof(d10_ini_data))
      d10_ini_scan(d10_ini_data, length, lv_cstr(r, a[0]), lv_cstr(r, a[1]), &hit);
    *out = hit.key_found ? lv_bytes(r, hit.value, hit.value_length) : a[2];
    return true;
  }
  if ((ln == LN_BAWRITEINI) && argc == 4) {
    const char *file = d10_path_base(lv_cstr(r, a[3]));
    const char *section = lv_cstr(r, a[0]);
    const char *key = lv_cstr(r, a[1]), *value = lv_cstr(r, a[2]);
    unsigned length = 0;
    if (!d->platform.read_file ||
        !d->platform.read_file(d->context, file, d10_ini_data,
                               sizeof(d10_ini_data), &length) ||
        length > sizeof(d10_ini_data))
      length = 0;
    d10_ini_hit_t hit;
    d10_ini_scan(d10_ini_data, length, section, key, &hit);
    // Read the join characters before any output copies: the append guard's
    // comparisons widen the compiler's index ranges past the input bounds.
    char data_tail = length ? d10_ini_data[length - 1] : '\n';
    char insert_tail = hit.section_insert && hit.section_insert <= length
                           ? d10_ini_data[hit.section_insert - 1]
                           : '\n';
    char line[192], header[96];
    int n = snprintf(line, sizeof(line), "%s=%s\r\n", key, value);
    int h = snprintf(header, sizeof(header), "[%s]\r\n", section);
    if (n < 0 || (unsigned)n >= sizeof(line) || h < 0 ||
        (unsigned)h >= sizeof(header)) {
      error(d, "ini entry capacity", key);
      return true;
    }
    unsigned used = 0;
    bool fits = true;
#define D10_APPEND(ptr, count)                                                \
  do {                                                                        \
    unsigned d10_count = (count);                                             \
    if (used + d10_count > sizeof(d10_ini_out))                               \
      fits = false;                                                           \
    else {                                                                    \
      memcpy(d10_ini_out + used, (ptr), d10_count);                           \
      used += d10_count;                                                      \
    }                                                                         \
  } while (0)
    if (hit.key_found) {
      D10_APPEND(d10_ini_data, hit.key_start);
      D10_APPEND(line, (unsigned)n);
      D10_APPEND(d10_ini_data + hit.key_end, length - hit.key_end);
    } else if (hit.section_found) {
      D10_APPEND(d10_ini_data, hit.section_insert);
      if (hit.section_insert && insert_tail != '\n') D10_APPEND("\r\n", 2);
      D10_APPEND(line, (unsigned)n);
      D10_APPEND(d10_ini_data + hit.section_insert, length - hit.section_insert);
    } else {
      D10_APPEND(d10_ini_data, length);
      if (length && data_tail != '\n') D10_APPEND("\r\n", 2);
      D10_APPEND(header, (unsigned)h);
      D10_APPEND(line, (unsigned)n);
    }
#undef D10_APPEND
    if (!fits) {
      error(d, "ini file capacity", file);
      return true;
    }
    *out = lv_num(d->platform.write_file &&
                  d->platform.write_file(d->context, file, d10_ini_out, used));
    return true;
  }
  if ((ln == LN_BAWRITEFILE) && argc == 2) {
    const char *file = d10_path_base(lv_cstr(r, a[0]));
    const char *text = lv_cstr(r, a[1]);
    *out = lv_num(d->platform.write_file &&
                  d->platform.write_file(d->context, file, text,
                                         (unsigned)strlen(text)));
    return true;
  }
  // Deleting a movie file the port ships is a no-op (Löwenzahn 7's photo
  // shop clears its scratch movie); a save file is emptied, which reads
  // back as absent.
  // baCopyFile(from, to, overwrite): a save file copies within the save
  // store; anything else (the disc's movies) answers success untouched.
  if ((ln == LN_BACOPYFILE) && argc >= 2) {
    unsigned length = 0;
    const char *from = lv_cstr(r, a[0]);
    if (d->platform.read_file && d->platform.write_file &&
        d->platform.read_file(d->context, d10_path_base(from), d10_ini_data, sizeof(d10_ini_data), &length) &&
        length) {
      char target[128];
      snprintf(target, sizeof(target), "%s", d10_path_base(lv_cstr(r, a[1])));
      d->platform.write_file(d->context, target, d10_ini_data, length);
    }
    *out = lv_num(0);
    return true;
  }
  // The stage window's handle, and blocking the mouse to it: Birthday's
  // Malefiz turns block clicks with a sprite of their own as well.
  if ((ln == LN_BASTAGEHANDLE) && argc == 0) { *out = lv_num(1); return true; }
  if ((ln == LN_BADISABLEMOUSE) && argc >= 1) { *out = lv_num(1); return true; }
  // File attributes have nothing to change in the save store; the open
  // file dialog answers as cancelled (Löwenzahn 7's photo import).
  if ((ln == LN_BASETFILEATTRIBUTES) && argc >= 1) { *out = lv_num(1); return true; }
  if ((ln == LN_BAGETFILENAME) && argc >= 1) { *out = lv_text(r, "", false); return true; }
  // inflate(rect, dx, dy) widens a rectangle on every side.
  if ((ln == LN_INFLATE) && argc == 3 && lv_type(a[0]) == LV_LIST) {
    double dx = lv_number(r, a[1]), dy = lv_number(r, a[2]);
    *out = lv_rect(r, lv_number(r, lv_at(r, a[0], 1)) - dx, lv_number(r, lv_at(r, a[0], 2)) - dy,
                   lv_number(r, lv_at(r, a[0], 3)) + dx, lv_number(r, lv_at(r, a[0], 4)) + dy);
    return true;
  }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // sprite.pointToWord(point) / pointToChar(point): the word or character
  // of a text sprite under a stage point, -1 where there is none (Optik's
  // lexicons find the link word clicked this way).
  if (((ln == LN_POINTTOWORD) || (ln == LN_POINTTOCHAR)) && argc == 2 && lv_type(a[0]) == LV_SPRITE) {
    unsigned channel = (unsigned)lv_id(a[0]);
    int index = d85_sprite_char(d, channel, a[1]);
    if (index < 0) { *out = lv_num(-1); return true; }
    if (ln == LN_POINTTOCHAR) { *out = lv_num(index + 1); return true; }
    const char *text = d85_member_text(d, dg_member(d, d->sprites[channel].value.member));
    size_t at = d85_char_byte(text, index);
    if (!text[at] || isspace((unsigned char)text[at])) { *out = lv_num(-1); return true; }
    int words = 0;
    bool in_word = false;
    for (size_t i = 0; i <= at; i++) {
      bool space = isspace((unsigned char)text[i]) != 0;
      if (!space && !in_word) words++;
      in_word = !space;
    }
    *out = lv_num(words);
    return true;
  }
  // locToCharPos(member, point): the character at a point of the member's
  // own box, 1-based (0 where there is none).
  if ((ln == LN_LOCTOCHARPOS) && argc == 2 && lv_type(a[0]) == LV_MEMBER && lv_type(a[1]) == LV_LIST) {
    int index = d->platform.text_char_at
                    ? d->platform.text_char_at(d->context, (uint32_t)lv_id(a[0]), 0,
                                               lv_integer(r, lv_at(r, a[1], 1)), lv_integer(r, lv_at(r, a[1], 2)))
                    : -1;
    *out = lv_num(index + 1);
    return true;
  }
  // chunkForeColor(member, kind, first, last): the palette colour of a text
  // chunk's first character, from the member's colour runs (the compiler
  // reads `the foreColor of word n of member m` this way).
  if ((ln == LN_CHUNKFORECOLOR) && argc == 4) {
    const dg_member_t *m = lv_type(a[0]) == LV_MEMBER || lv_type(a[0]) == LV_FIELD
                               ? dg_member(d, (uint32_t)lv_id(a[0])) : NULL;
    if (!m) { *out = lv_num(0); return true; }
    const char *text = d85_member_text(d, m);
    size_t start = 0, end = 0;
    lv_t source = lv_text(r, text, false);
    if (!lv_chunk_range(r, lv_cstr(r, a[1]), source, a[2], a[3], &start, &end)) { *out = lv_num(0); return true; }
    uint32_t rgb = m->text_style ? m->text_style->color : 0;
    const dg_text_extra_t *extra = dg_text_extra(d, m->id);
    int index = d85_byte_char(text, start);
    for (unsigned i = 0; extra && i < extra->run_count && extra->runs[i].offset <= (uint32_t)index; i++)
      rgb = extra->runs[i].rgb;
    *out = lv_num((int32_t)d85_nearest_index(d, rgb));
    return true;
  }
#endif
  // sleep(ms) would stall the whole player; the port moves on at once.
  if ((ln == LN_SLEEP) && argc == 1) return true;
  if ((ln == LN_BADELETEFILE) && argc == 1) {
    const char *path = lv_cstr(r, a[0]);
    if (d10_ends_ci(path, ".dxr") || d10_ends_ci(path, ".dir") || d10_ends_ci(path, ".cxt")) {
      *out = lv_num(1);
      return true;
    }
    *out = lv_num(d->platform.write_file &&
                  d->platform.write_file(d->context, d10_path_base(path), "", 0));
    return true;
  }
  if ((ln == LN_BAFILEEXISTS) && argc == 1) {
    const char *path = lv_cstr(r, a[0]);
    // Converted movie files always ship in the package; navigation still fails
    // loudly for a genuinely absent overlay. Preference files answer from
    // the archive.
    if (d10_ends_ci(path, ".dxr") || d10_ends_ci(path, ".dir") ||
        d10_ends_ci(path, ".cxt")) {
      *out = lv_num(1);
      return true;
    }
    unsigned length = 0;
    // An archive slot never written answers empty: no such file yet.
    *out = lv_num(d->platform.read_file &&
                  d->platform.read_file(d->context, d10_path_base(path),
                                        d10_ini_data, sizeof(d10_ini_data),
                                        &length) && length);
    return true;
  }
  if ((ln == LN_BACREATEFOLDER) || (ln == LN_BAFOLDEREXISTS) ||
      (ln == LN_BASETFILEPERMISSIONS) || (ln == LN_BAFLUSHINI) ||
      (ln == LN_BAADMINISTRATOR) || (ln == LN_BAPLACECURSOR) ||
      (ln == LN_BARESTRICTCURSOR) || (ln == LN_BAFREECURSOR) ||
      (ln == LN_BAWINHANDLE) || (ln == LN_BASETDISPLAY) ||
      (ln == LN_BASETDISPLAYEX) || (ln == LN_BAACTIVEWINDOW)) {
    *out = lv_num(1);
    return true;
  }
  if (ln == LN_BAFINDWINDOW) { *out = lv_num(0); return true; }
  // A processor past every authored slow-machine threshold (Birthday trims
  // animations below 751 MHz).
  if ((ln == LN_BACPUINFO) && argc == 1) {
    const char *item = lv_cstr(r, a[0]);
    *out = d10_span_ci(item, (unsigned)strlen(item), "speed") ? lv_num(1000)
                                                             : lv_text(r, "Pentium", false);
    return true;
  }
  // The disc's folders hold no developer tools: Birthday's debug menu
  // lists none.
  if ((ln == LN_BAFOLDERLIST) || (ln == LN_BAFILELIST)) {
    *out = lv_list(r, 0, NULL, false);
    return true;
  }
  if (ln == LN_BAGETVOLUME) { *out = lv_num(100); return true; }
  if (ln == LN_BAVERSION) { *out = lv_text(r, "winxp", false); return true; }
  if (ln == LN_BAWINDOWINFO) { *out = lv_text(r, "normal", false); return true; }
  if (ln == LN_BADISKINFO) { *out = lv_text(r, "fixed", false); return true; }
  if (ln == LN_BASYSFOLDER) { *out = lv_text(r, "C:\\", false); return true; }
  if (ln == LN_BAPROMPT) { *out = lv_text(r, "", false); return true; }
  if (ln == LN_BAMSGBOX) { *out = lv_text(r, "Yes", false); return true; }
  if ((ln == LN_BAREADREGSTRING) && argc >= 3) { *out = a[2]; return true; }
  if ((ln == LN_BAMULTIDISPLAYINFO) && argc == 2) {
    const char *item = lv_cstr(r, a[1]);
    const char *answer = d10_span_ci(item, (unsigned)strlen(item), "width")    ? "800"
                         : d10_span_ci(item, (unsigned)strlen(item), "height") ? "600"
                         : d10_span_ci(item, (unsigned)strlen(item), "depth")  ? "32"
                         : d10_span_ci(item, (unsigned)strlen(item), "refresh") ? "60"
                                                                                : "";
    *out = lv_text(r, answer, false);
    return true;
  }
  if ((ln == LN_BAOPENFILE) && argc >= 1) {
    // Launching an external viewer has no target equivalent; keep visible.
    if (d->platform.trace) {
      char text[128];
      snprintf(text, sizeof(text), "OPEN_FILE_PENDING %s", lv_cstr(r, a[0]));
      d->platform.trace(d->context, text);
    }
    *out = lv_num(0);
    return true;
  }
  if (!strncmp(name, "fx_", 3) && argc >= 1) {
    // FileXtra4 surface, truthful to the target: the packaged files are
    // complete and read-only, saves live in the save image behind the archive, no
    // removable volumes exist, and native dialogs cannot open (authored
    // code treats an empty dialog result as user cancellation).
    if (ln == LN_FX_VOLUMESTOLIST) { *out = lv_list(r, 0, NULL, false); return true; }
    if ((ln == LN_FX_VOLUMEISCDROM) || (ln == LN_FX_VOLUMEISREMOVABLE)) {
      *out = lv_num(0);
      return true;
    }
    if (ln == LN_FX_VOLUMEGETFREEBYTES) { *out = lv_num(4 * 1024 * 1024); return true; }
    if ((ln == LN_FX_FILEEXISTS) || (ln == LN_FX_FOLDEREXISTS) ||
        (ln == LN_FX_FOLDERCREATE) || (ln == LN_FX_FILESETWRITESTATE) ||
        (ln == LN_FX_FILEGETWRITESTATE)) {
      *out = lv_num(1);
      return true;
    }
    if (ln == LN_FX_FILEDELETE) {
      // A zero-length archive write is deletion: reads answer absent.
      bool ok = argc >= 2 && d->platform.write_file &&
                d->platform.write_file(d->context,
                                       d10_path_base(lv_cstr(r, a[1])), "", 0);
      *out = lv_num(ok);
      return true;
    }
    if (ln == LN_FX_FOLDERGETSPECIALPATH) { *out = lv_text(r, "C:\\", false); return true; }
    if ((ln == LN_FX_FILESAVEASDIALOG) || (ln == LN_FX_FILEOPENDIALOG) ||
        (ln == LN_FX_ERRORSTRING)) {
      *out = lv_text(r, "", false);
      return true;
    }
    if (ln == LN_FX_ERRORNUMBER) { *out = lv_num(0); return true; }
  }
  // Remaining Flash sprite playback methods on the flattened film timeline.
  if (((ln == LN_REWIND) || (ln == LN_PLAY)) && argc == 1 &&
      lv_type(a[0]) == LV_SPRITE) {
    unsigned n = (unsigned)lv_id(a[0]);
    if ((ln == LN_REWIND) && n < DG_SPRITES) {
      d->sprites[n].film_frame = 0;
      flash_seek(d,&d->sprites[n],dg_member(d,d->sprites[n].value.member));
      dg_sprite_changed(d, n);
    }
    if (n < DG_SPRITES && ln == LN_PLAY) {
      d->sprites[n].film_paused = false;
      dg_flash_play(d,n,true);
    }
    return true;
  }
  if (((ln == LN_GOTOANDSTOP) || (ln == LN_GOTOANDPLAY)) &&
      argc == 2 && lv_type(a[0]) == LV_SPRITE) {
    unsigned n = (unsigned)lv_id(a[0]);
    if (n < DG_SPRITES)
      d->sprites[n].film_paused = ln == LN_GOTOANDSTOP;
    return d10_call(d, "sprite_go", 2, a, out);
  }
  // The two MX 2004 JavaScript-dialect handlers the decompiler cannot
  // translate, recovered from their compiled literal pools and lowered by
  // the game's pinned compatibility transform.
  if ((ln == LN_JS_INT2HEX) && argc == 1) {
    // function int2hex(num) { return num.toString(16) } — JS lowercase hex.
    long value = (long)lv_integer(r, a[0]);
    char text[24];
    if (value < 0)
      snprintf(text, sizeof(text), "-%lx", -value);
    else
      snprintf(text, sizeof(text), "%lx", value);
    *out = lv_text(r, text, false);
    return true;
  }
  if ((ln == LN_JS_CLEARGARBAGE) && argc == 0) {
    // function clearGarbage() { _system.gc() } — collection runs between
    // interpreter steps and on allocation pressure; the call is served.
    return true;
  }
  if ((ln == LN_UNDECOMPILED_BYTECODE) && argc == 1) {
    // The pinned decompiler could not translate this handler's bytecode.
    // Reproduce the alert-and-continue script failure, never silence.
    script_error(d, "undecompiled bytecode", lv_cstr(r, a[0]));
    return true;
  }
  if (argc >= 1 && lv_type(a[0]) == LV_WINDOW) {
    lv_t ignored = {0};
    if (d10_get(d, name, a[0], &ignored)) { *out = ignored; return true; }
  }
  return false;
}
