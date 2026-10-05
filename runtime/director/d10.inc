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
      if (m->type && m->id >> 20 == lib->file) {
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
            *id = lib_file << 20 | lib_cast << 16 | file->members[i].number;
            return 1;
          }
      } else if (cast_name) {
        unsigned number = (unsigned)lv_integer(r, value);
        if (number) {
          *id = lib_file << 20 | lib_cast << 16 | number;
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
    if (lv_start_method(r, r->roots[DG_TIMEOUT_ROOT + i],
                        d->timeouts[i].handler, 1, &ref))
      return true;
  }
  return false;
}
// A Flash sprite exposes the objects inside its timeline to Lingo. The
// conversion flattens each Flash asset to a film timeline, so there is no
// player holding that object model: an addressed object answers a property
// bag. A name matching a recorded edit-text field seeds from that field's
// descriptor and its text renders as an overlay above the flattened frame;
// any other name seeds from the sprite's own geometry and stays a recorded
// approximation, traced so the evidence shows which objects it covers.
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
static bool d10_flash_get(dg_runtime_t *d, const char *name, lv_t owner,
                          lv_t *out) {
  if (lv_type(owner) != LV_SPRITE || lv_id(owner) < 1 || lv_id(owner) >= DG_SPRITES)
    return false;
  unsigned sprite = (unsigned)lv_id(owner);
  const dg_member_t *m = dg_member(d, d->sprites[sprite].value.member);
  if (!m || m->source_xtra != 1 || !*name || *name == '_')
    return false;
  lv_runtime_t *r = d->values;
  for (unsigned i = 0; i < d->flash_object_count; i++)
    if (d->flash_objects[i].sprite == sprite &&
        eq(d->flash_objects[i].name, name)) {
      *out = r->roots[DG_FLASH_ROOT + i];
      return true;
    }
  if (d->flash_object_count == DG_FLASH_OBJECTS) {
    error(d, "flash object capacity", name);
    return true;
  }
  const dg_flash_field_t *field = dg_flash_field_find(m, name);
  lv_t object;
  if (field) {
    unsigned height = dg_flash_field_wrap(field, 0, field->text, field->width,
                                          d10_wrap_scratch,
                                          sizeof(d10_wrap_scratch));
    lv_t pairs[] = {
        lv_text(r, "_x", true),          lv_num(field->x),
        lv_text(r, "_y", true),          lv_num(field->y),
        lv_text(r, "_width", true),      lv_num(field->width),
        lv_text(r, "_height", true),     lv_num(field->height),
        lv_text(r, "_visible", true),    lv_num(1),
        lv_text(r, "size", true),        lv_num(field->style->size),
        lv_text(r, "color", true),
        lv_make(LV_COLOR, (int32_t)field->style->color),
        lv_text(r, "text", true),        lv_text(r, field->text, false),
        lv_text(r, "textheight", true),  lv_num(height),
    };
    object = lv_list(r, sizeof(pairs) / sizeof(*pairs), pairs, true);
  } else {
    int l, t, right, bottom;
    dg_bounds(d, sprite, &l, &t, &right, &bottom);
    lv_t pairs[] = {
        lv_text(r, "_x", true),       lv_num(l),
        lv_text(r, "_y", true),       lv_num(t),
        lv_text(r, "_width", true),   lv_num(right - l),
        lv_text(r, "_height", true),  lv_num(bottom - t),
        lv_text(r, "_visible", true), lv_num(1),
        lv_text(r, "text", true),     lv_text(r, "", false),
    };
    object = lv_list(r, sizeof(pairs) / sizeof(*pairs), pairs, true);
  }
  if (r->failed)
    return true;
  unsigned slot = d->flash_object_count++;
  d->flash_objects[slot].sprite = sprite;
  snprintf(d->flash_objects[slot].name, sizeof(d->flash_objects[slot].name),
           "%s", name);
  r->roots[DG_FLASH_ROOT + slot] = object;
  if (d->platform.trace) {
    char text[96];
    snprintf(text, sizeof(text),
             field ? "FLASH_FIELD_OBJECT sprite=%u name=%s"
                   : "FLASH_OBJECT_PENDING sprite=%u name=%s",
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
    if (lv_id(d->values->roots[DG_FLASH_ROOT + i]) == lv_id(value))
      return i + 1;
  return 0;
}
static bool d10_get(dg_runtime_t *d, const char *name, lv_t owner, lv_t *out) {
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
  if (lv_type(owner) == LV_VOID) {
    // Fixed environment answers for the 8 MiB target; the memory monitor
    // renders them, it never branches on them.
    if (ln == LN_MEMORYSIZE) { *out = lv_num(8 * 1024 * 1024); return true; }
    if ((ln == LN_FREEBYTES) || (ln == LN_FREEBLOCK)) {
      *out = lv_num(4 * 1024 * 1024);
      return true;
    }
    if (ln == LN_ACTORLIST) {
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
      lv_t rect = lv_rect(r, 0, 0, 800, 600);
      *out = lv_list(r, 4,
                     (lv_t[]){lv_text(r, "rect", true), rect,
                              lv_text(r, "sourcerect", true), rect}, true);
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
    *out = lv_num(m && m->film_count > 1 && d->sprites[n].film_frame + 1 < m->film_count);
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
      if (m->type && m->id >> 20 == lib_file && m->cast == lib_cast &&
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
    if (ln == LN_NAME) { *out = lv_text(r, d->timeouts[slot].name, false); return true; }
    if (ln == LN_TARGET) { *out = r->roots[DG_TIMEOUT_ROOT + slot]; return true; }
    if (ln == LN_FORGET) {
      d->timeouts[slot].active = false;
      r->roots[DG_TIMEOUT_ROOT + slot] = (lv_t){0};
      *out = (lv_t){0};
      return true;
    }
  }
  if (lv_type(owner) == LV_MEMBER && (ln == LN_IMAGE)) {
    *out = lv_make(LV_IMAGE, lv_id(owner));
    return true;
  }
  if (lv_type(owner) == LV_IMAGE) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (ln == LN_RECT) {
      *out = lv_rect(r, 0, 0, m ? m->width : 0, m ? m->height : 0);
      return true;
    }
    if (ln == LN_WIDTH) { *out = lv_num(m ? m->width : 0); return true; }
    if (ln == LN_HEIGHT) { *out = lv_num(m ? m->height : 0); return true; }
  }
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
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
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
    if (d->platform.trace) {
      char text[96];
      snprintf(text, sizeof(text), "CAST_SWAP %s %s", lib->name, target->code->name);
      d->platform.trace(d->context, text);
    }
    return true;
  }
  if (lv_type(owner) == LV_MEMBER && (ln == LN_IMAGE)) {
    // Assigning a mutated surface back; the mutation itself was traced.
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
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
  if ((ln == LN_WINDOW) && argc == 1) {
    if (!d10_stem_matches("CONTROL", lv_cstr(r, a[0]))) {
      script_error(d, "unsupported movie window", lv_cstr(r, a[0]));
      return true;
    }
    *out = lv_make(LV_WINDOW, 1);
    return true;
  }
  if ((ln == LN_TELL_WINDOW) && argc == 1) { d->tell_window = true; return true; }
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
  if ((ln == LN_NEW) && argc == 4 && lv_type(a[0]) == LV_TIMEOUT) {
    unsigned slot = (unsigned)lv_id(a[0]) - 1;
    if (slot >= DG_TIMEOUTS) return false;
    uint32_t ms = (uint32_t)lv_integer(r, a[1]);
    snprintf(d->timeouts[slot].handler, sizeof(d->timeouts[slot].handler),
             "%s", lv_cstr(r, a[2]));
    d->timeouts[slot].period = ms;
    d->timeouts[slot].next = d->ticks + (ms * 60u + 999u) / 1000u;
    d->timeouts[slot].active = true;
    d->timeouts[slot].persistent = false;
    r->roots[DG_TIMEOUT_ROOT + slot] = a[3];
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
  // Memory preloading is a no-op: ROM assets stream on demand.
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
  // Sprite tells drive a flattened film timeline: one-based frames over the
  // sprite's film assets, holding past the end exactly as film advancement.
  if (((ln == LN_SPRITE_FRAME) || (ln == LN_SPRITE_LASTFRAME)) && argc == 1) {
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
    if (n < DG_SPRITES && frame >= 1 &&
        d->sprites[n].film_frame != (unsigned)frame - 1) {
      d->sprites[n].film_frame = (unsigned)frame - 1;
      dg_sprite_changed(d, n);
    }
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
  // Imaging Lingo on member surfaces: reads answer the member's geometry,
  // mutations stay traced approximations until a native compositor exists.
  if (argc >= 1 && lv_type(a[0]) == LV_IMAGE) {
    if (ln == LN_DUPLICATE) { *out = a[0]; return true; }
    if ((ln == LN_SETALPHA) || (ln == LN_FILL) ||
        (ln == LN_COPYPIXELS) || (ln == LN_DRAW) ||
        (ln == LN_SETPIXEL)) {
      if (d->platform.trace) {
        char text[96];
        snprintf(text, sizeof(text), "IMAGE_OP_PENDING %s member=%u", name,
                 (unsigned)lv_id(a[0]) & 65535);
        d->platform.trace(d->context, text);
      }
      *out = lv_num(1);
      return true;
    }
    if (ln == LN_GETPIXEL) { *out = lv_make(LV_COLOR, 0); return true; }
  }
  // Flash sprite playback methods on flattened film timelines. stop() holds
  // the final pose: authored code stops after seeking near the end, and the
  // flattened timeline has no independent clock to pause mid-flight.
  if ((ln == LN_GOTOFRAME) && argc == 2 && lv_type(a[0]) == LV_SPRITE)
    return d10_call(d, "sprite_go", 2, a, out);
  if ((ln == LN_STOP) && argc == 1 && lv_type(a[0]) == LV_SPRITE) {
    unsigned n = (unsigned)lv_id(a[0]);
    const dg_member_t *m = n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
    if (m && m->film_count && d->sprites[n].film_frame + 1 < m->film_count) {
      d->sprites[n].film_frame = (unsigned)m->film_count - 1;
      dg_sprite_changed(d, n);
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
    if (d->platform.play_file)
      d->platform.play_file(d->context, (unsigned)channel - 1, virtual_name);
    else if (d->platform.trace) {
      char text[128];
      snprintf(text, sizeof(text), "PLAY_FILE channel=%ld name=%s", channel, virtual_name);
      d->platform.trace(d->context, text);
    }
    return true;
  }
  // Buddy API surface. The projector used it for preference INI files (the
  // FlashRAM archive serves those), display management (the target renders
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
  // setFlashProperty / getFlashProperty address a named object inside the
  // timeline. The store reads back; only a field's _visible changes what
  // renders — other named children stay recorded approximations, traced
  // when the object bag is created.
  if (((ln == LN_SETFLASHPROPERTY) || (ln == LN_GETFLASHPROPERTY)) &&
      argc >= 3 && lv_type(a[0]) == LV_SPRITE) {
    char key[32];
    snprintf(key, sizeof(key), "_%s", lv_cstr(r, a[2]));
    for (char *p = key; *p; p++)
      if (*p >= 'A' && *p <= 'Z') *p += 32;
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
  if ((ln == LN_BAFILEEXISTS) && argc == 1) {
    const char *path = lv_cstr(r, a[0]);
    // Converted movie files always ship on the ROM; navigation still fails
    // loudly for a genuinely absent overlay. Preference files answer from
    // the archive.
    if (d10_ends_ci(path, ".dxr") || d10_ends_ci(path, ".dir") ||
        d10_ends_ci(path, ".cxt")) {
      *out = lv_num(1);
      return true;
    }
    unsigned length = 0;
    *out = lv_num(d->platform.read_file &&
                  d->platform.read_file(d->context, d10_path_base(path),
                                        d10_ini_data, sizeof(d10_ini_data),
                                        &length));
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
    // FileXtra4 surface, truthful to the target: the ROM filesystem is
    // complete and read-only, saves live in FlashRAM behind the archive, no
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
    if ((ln == LN_REWIND) && n < DG_SPRITES &&
        d->sprites[n].film_frame) {
      d->sprites[n].film_frame = 0;
      dg_sprite_changed(d, n);
    }
    // play resumes with the score clock, which never stopped.
    return true;
  }
  if (((ln == LN_GOTOANDSTOP) || (ln == LN_GOTOANDPLAY)) &&
      argc == 2 && lv_type(a[0]) == LV_SPRITE)
    return d10_call(d, "sprite_go", 2, a, out);
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
