// Embedded Director movies own their score/playhead and suspended call stack,
// while sharing the host's global table, objects and symbols. Inactive roots
// remain registered with the shared heap's collector. Behavioral reference:
// Macromedia MX2004 Using Director p202: Enable Scripts enables cast and
// sprite handlers; frame/movie scripts do not run. The host controls tempo,
// transitions and palette. Child playheads advance with the host's frames.
// https://dit.uoi.gr/files/director_mx_2004using_director_compressed-151-350.pdf
#include "linked_geometry.h"
#define DG_LINKED_LIMIT 8u
#define DG_LINKED_DEPTH 4u
typedef struct dg_linked_state {
  lv_root_span_t roots_span, stack_span, self_span;
  lv_t roots[LV_ROOTS], stack[LV_STACK_SLOTS], self[LV_FRAMES + 2];
  lv_frame_t frames[LV_FRAMES];
  unsigned depth, shared_count;
  const lv_movie_t *current, *shared[LV_SHARED];
  lv_t result, the_result;
  bool yielded, aborted;
} dg_linked_state_t;
typedef struct dg_linked_child {
  struct dg_linked_child *next;
  dg_runtime_t *runtime;
  unsigned channel;
  uint32_t member;
} dg_linked_child_t;
// Global script/member references may point into a parent or sibling movie.
// Resolve their immutable descriptors without changing implicit movie lookup.
static const dg_movie_t *linked_loaded(const dg_runtime_t *d, unsigned file, const lv_movie_t *code) {
  for (unsigned i = 0; i < d->loaded_count; i++)
    if ((file && d->loaded[i]->id == file) || (code && d->loaded[i]->code == code)) return d->loaded[i];
  for (const dg_linked_child_t *c = d->linked_children; c; c = c->next) {
    const dg_movie_t *found = linked_loaded(c->runtime, file, code);
    if (found) return found;
  }
  return NULL;
}
static bool linked_state_new(dg_runtime_t *d) {
  if (d->linked_state) return true;
  dg_linked_state_t *s = calloc(1, sizeof(*s));
  if (!s) { lv_fail(d->values, "linked movie interpreter allocation failed"); return false; }
  d->linked_state = s;
  s->roots_span = (lv_root_span_t){.next = d->values->external_roots, .values = s->roots, .count = LV_ROOTS};
  s->stack_span = (lv_root_span_t){.next = &s->roots_span, .values = s->stack};
  s->self_span = (lv_root_span_t){.next = &s->stack_span, .values = s->self};
  d->values->external_roots = &s->self_span;
  return true;
}
static void linked_save(dg_runtime_t *d) {
  lv_runtime_t *r = d->values;
  dg_linked_state_t *s = d->linked_state;
  s->depth = r->depth; s->current = r->current; s->shared_count = r->shared_count;
  s->result = r->result; s->the_result = r->the_result;
  s->yielded = r->yielded; s->aborted = r->aborted;
  memcpy(s->roots, r->roots, sizeof(s->roots));
  memcpy(s->shared, r->shared, sizeof(s->shared));
  memcpy(s->frames, r->frames, r->depth * sizeof(lv_frame_t));
  unsigned used = 0;
  for (unsigned i = 0; i < r->depth; i++) {
    const lv_frame_t *f = &r->frames[i];
    unsigned end = (unsigned)(f->temps - r->stack) + (f->temp_count > f->temp_extent ? f->temp_count : f->temp_extent);
    if (used < end) used = end;
    s->self[i] = f->self;
  }
  memcpy(s->stack, r->stack, used * sizeof(lv_t));
  s->stack_span.count = used;
  s->self[r->depth] = r->result; s->self[r->depth + 1] = r->the_result;
  s->self_span.count = r->depth + 2;
}
static void linked_restore(dg_runtime_t *d) {
  lv_runtime_t *r = d->values;
  dg_linked_state_t *s = d->linked_state;
  r->depth = s->depth; r->current = s->current; r->shared_count = s->shared_count;
  r->result = s->result; r->the_result = s->the_result;
  r->yielded = s->yielded; r->aborted = s->aborted; r->context = d;
  memcpy(r->roots, s->roots, sizeof(s->roots));
  memcpy(r->shared, s->shared, sizeof(s->shared));
  memcpy(r->frames, s->frames, s->depth * sizeof(lv_frame_t));
  memcpy(r->stack, s->stack, s->stack_span.count * sizeof(lv_t));
  // Resolution caches depend on which movies supply script/global handlers.
  memset(r->call_cache, 0, sizeof(r->call_cache));
  r->call_generation++; r->name_id_for = NULL; r->symbol_for = NULL;
}
static dg_linked_child_t *linked_find(const dg_runtime_t *d, unsigned channel) {
  for (dg_linked_child_t *c = d->linked_children; c; c = c->next)
    if (c->channel == channel) return c;
  return NULL;
}
const dg_runtime_t *dg_linked_movie(const dg_runtime_t *d, unsigned channel) {
  dg_linked_child_t *child = linked_find(d, channel);
  return child ? child->runtime : NULL;
}
dg_runtime_t *dg_linked_begin(dg_runtime_t *d, unsigned channel) {
  if (d->values->context != d) { lv_fail(d->values, "linked movie context mismatch"); return NULL; }
  dg_linked_child_t *child = linked_find(d, channel);
  if (!child || !d->linked_state) return NULL;
  linked_save(d); linked_restore(child->runtime);
  return child->runtime;
}
void dg_linked_end(dg_runtime_t *d) {
  dg_runtime_t *child = d->values->context;
  if (child == d || !d->linked_state) return;
  if (!child || child->linked_parent != d) { lv_fail(d->values, "linked movie restore mismatch"); return; }
  linked_save(child); linked_restore(d);
}
static void linked_unroot(lv_runtime_t *r, lv_root_span_t *span) {
  lv_root_span_t **p = &r->external_roots;
  while (*p && *p != span) p = &(*p)->next;
  if (*p) *p = span->next;
}
void dg_linked_dispose(dg_runtime_t *d) {
  dg_flash_dispose(d);
  if (d->linked_parent) dg_stop_sounds(d);
  while (d->linked_children) {
    dg_linked_child_t *c = d->linked_children;
    d->linked_children = c->next;
    dg_linked_dispose(c->runtime);
    dg_films_dispose(c->runtime);
    dg_images_dispose(c->runtime);
    free(c->runtime); free(c);
  }
  dg_linked_state_t *s = d->linked_state;
  if (s) {
    linked_unroot(d->values, &s->self_span);
    linked_unroot(d->values, &s->stack_span);
    linked_unroot(d->values, &s->roots_span);
    free(s); d->linked_state = NULL;
  }
}
static bool linked_libraries_walk(dg_runtime_t *parent, const dg_movie_t *movie,
                                  unsigned root, const dg_movie_t **shared, unsigned *count) {
  for (unsigned i = 0; i < movie->cast_count; i++) {
    unsigned id = movie->casts[i].file;
    if (!id || id == movie->id || id == root) continue;
    bool found = false;
    for (unsigned j = 0; j < *count; j++) if (shared[j]->id == id) found = true;
    if (found) continue;
    if (*count >= DG_FILES - 1 || *count >= LV_SHARED) {
      lv_fail(parent->values, "linked movie external cast capacity"); return false;
    }
    const dg_movie_t *cast = parent->platform.find_movie ? parent->platform.find_movie(parent->context, NULL, id) : NULL;
    if (!cast) { lv_fail(parent->values, "linked movie external cast missing"); return false; }
    shared[(*count)++] = cast;
    if (!linked_libraries_walk(parent, cast, root, shared, count)) return false;
  }
  return true;
}
static bool linked_libraries(dg_runtime_t *parent, const dg_movie_t *movie,
                              const dg_movie_t **shared, unsigned *count) {
  return linked_libraries_walk(parent, movie, movie->id, shared, count);
}
static unsigned linked_count(const dg_runtime_t *d) {
  unsigned count = 0;
  for (const dg_linked_child_t *c = d->linked_children; c; c = c->next)
    count += 1 + linked_count(c->runtime);
  return count;
}
static dg_linked_child_t *linked_create(dg_runtime_t *parent, unsigned channel, const dg_member_t *member) {
  if (!member->width || !member->height) {
    lv_fail(parent->values, "linked movie has empty bounds"); return NULL;
  }
  dg_runtime_t *host = parent;
  while (host->linked_parent) host = host->linked_parent;
  if (host->linked_next_instance >= UINT32_MAX / DG_SPRITES - 1) {
    lv_fail(parent->values, "linked movie instance identity exhausted"); return NULL;
  }
  if (linked_count(host) >= DG_LINKED_LIMIT || parent->linked_depth >= DG_LINKED_DEPTH) {
    lv_fail(parent->values, "linked movie nesting or instance capacity exceeded"); return NULL;
  }
  const dg_movie_t *movie = parent->platform.find_movie && member->linked_movie
      ? parent->platform.find_movie(parent->context, NULL, member->linked_movie) : NULL;
  if (!movie) { lv_fail(parent->values, "linked movie target unavailable"); return NULL; }
  for (const dg_runtime_t *p = parent; p; p = p->linked_parent)
    if (p->movie && p->movie->id == movie->id) { lv_fail(parent->values, "recursive linked movie"); return NULL; }
  const dg_movie_t *shared[DG_FILES]; unsigned shared_count = 0;
  if (!linked_libraries(parent, movie, shared, &shared_count) || !linked_state_new(parent)) return NULL;
  dg_linked_child_t *entry = calloc(1, sizeof(*entry));
  dg_runtime_t *child = entry ? calloc(1, sizeof(*child)) : NULL;
  if (!child) { free(entry); lv_fail(parent->values, "linked movie allocation failed"); return NULL; }
  child->values = parent->values; child->platform = parent->platform; child->context = parent->context;
  child->linked_parent = parent; child->linked_depth = parent->linked_depth + 1;
  child->linked_instance = ++host->linked_next_instance;
  child->linked_flags = member->linked_flags;
  child->ticks = parent->ticks; child->clock = parent->clock;
  child->sound_level = parent->sound_level; child->color_depth = parent->color_depth;
  child->float_precision = parent->float_precision;
  for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++) child->channel_volume[ch] = parent->channel_volume[ch];
  if (!(member->linked_flags & 8)) {
    child->platform.sound = NULL; child->platform.sound_busy = NULL;
    child->platform.sound_pause = NULL; child->platform.sound_position = NULL;
    child->platform.sound_break_loop = NULL;
    child->platform.play_file = NULL;
  }
  if (!linked_state_new(child)) { free(child); free(entry); return NULL; }
  entry->runtime = child; entry->channel = channel; entry->member = member->id;
  entry->next = parent->linked_children; parent->linked_children = entry;
  linked_save(parent); linked_restore(child);
  bool ok = dg_enter(child, movie, 1, shared, shared_count);
  linked_save(child); linked_restore(parent);
  if (!ok) return NULL;
  parent->visual_revision++;
  return entry;
}
static void linked_remove(dg_runtime_t *parent, dg_linked_child_t **link) {
  dg_linked_child_t *entry = *link;
  // Finish the child's sprite/movie lifecycle in its own context before its
  // roots and frame continuations are released. Suspended handlers are ended.
  dg_runtime_t *child = dg_linked_begin(parent, entry->channel);
  if (child) {
    child->values->depth = 0;
    for (unsigned i = 1; i < DG_SPRITES && !child->values->failed; i++) {
      if (child->sprites[i].value.type && sprite_event(child, "endsprite", i, 0))
        lv_run(child->values, DG_SERVICE_BUDGET);
      child->values->depth = 0;
    }
    if (dg_event(child, "stopmovie", 0)) lv_run(child->values, DG_SERVICE_BUDGET);
    child->values->depth = 0;
    dg_stop_sounds(child);
    dg_linked_end(parent);
  }
  *link = entry->next;
  if (parent->linked_focus == entry->channel) parent->linked_focus = 0;
  dg_linked_dispose(entry->runtime); dg_films_dispose(entry->runtime); dg_images_dispose(entry->runtime);
  free(entry->runtime); free(entry); parent->visual_revision++;
}
static void linked_reset(dg_runtime_t *d) {
  while (d->linked_children) linked_remove(d, &d->linked_children);
}
static bool linked_update(dg_runtime_t *parent, unsigned budget) {
  lv_runtime_t *r = parent->values;
  bool any = false;
  for (unsigned word = 0; word < (DG_SPRITES + 31) / 32; word++) any |= parent->linked_channels[word] != 0;
  if (!any && !parent->linked_children) return !r->failed;
  dg_linked_child_t **link = &parent->linked_children;
  while (*link) {
    dg_linked_child_t *entry = *link;
    const dg_sprite_t *s = &parent->sprites[entry->channel];
    const dg_member_t *m = dg_member(parent, s->value.member);
    if (!s->value.type || !s->visible || !m || m->type != 9 || m->id != entry->member)
      linked_remove(parent, link);
    else link = &entry->next;
  }
  for (unsigned i = 1; i < DG_SPRITES && !r->failed; i++) {
    if (!(parent->linked_channels[i >> 5] & (UINT32_C(1) << (i & 31)))) continue;
    const dg_sprite_t *s = &parent->sprites[i];
    if (!s->visible || !s->value.type) continue;
    const dg_member_t *m = dg_member(parent, s->value.member);
    if (!m || m->type != 9) continue;
    dg_linked_child_t *entry = linked_find(parent, i);
    if (!entry) entry = linked_create(parent, i, m);
    if (!entry) break;
    float quad[8], u = 0, v = 0;
    dg_sprite_quad(parent, s, quad);
    bool inside = dg_quad_uv(quad, parent->mouse_x, parent->mouse_y, &u, &v) &&
                  dg_hit(parent, parent->mouse_x, parent->mouse_y) == i;
    if (s->flip_h) u = 1 - u;
    if (s->flip_v) v = 1 - v;
    inside = inside && dg_linked_uv(m, s, &u, &v);
    int x = inside ? m->linked_left + (int)fminf(m->width - 1, u * m->width) : -32768;
    int y = inside ? m->linked_top + (int)fminf(m->height - 1, v * m->height) : -32768;
    dg_runtime_t *child = dg_linked_begin(parent, i);
    if (!child) break;
    if (parent->linked_focus == i && parent->mouse_down && !child->mouse_down && !inside)
      parent->linked_focus = 0;
    unsigned revision = child->visual_revision;
    uint16_t previous_volume[DG_SOUND_CHANNELS];
    memcpy(previous_volume, child->channel_volume, sizeof(previous_volume));
    bool paused = s->film_paused || child->linked_paused;
    // Stopping the playhead retains sprite interaction. Pin its host-frame
    // serial while stopped, rather than suspending the entire interpreter.
    if (paused) child->linked_frame_serial = parent->frame_serial;
    {
      if (child->ticks < parent->ticks) {
        child->ticks = parent->ticks - 1; child->clock = parent->clock - 1000000;
        dg_tick(child, x, y, (inside || child->mouse_down) && parent->mouse_down, budget);
      } else dg_service(child, budget);
      if (child->next_movie[0] && dg_transition_ready(child)) {
        char stem[sizeof(child->next_movie)];
        snprintf(stem, sizeof(stem), "%s", basename_(child->next_movie));
        char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
        const dg_movie_t *next = parent->platform.find_movie ? parent->platform.find_movie(parent->context, stem, 0) : NULL;
        const dg_movie_t *shared[DG_FILES]; unsigned count = 0;
        if (!next || !linked_libraries(parent, next, shared, &count)) lv_fail(r, "linked movie transition target missing");
        else dg_enter(child, next, child->next_movie_frame, shared, count);
      }
      dg_update_stage(child);
    }
    if (child->linked_flags & 8)
      for (unsigned ch = 0; ch < DG_SOUND_CHANNELS; ch++)
        if (child->channel_volume[ch] != previous_volume[ch] &&
            sound_host(child)->linked_sound_owner[ch] == child)
          parent->channel_volume[ch] = child->channel_volume[ch];
    dg_linked_end(parent);
    if (child->visual_revision != revision) parent->visual_revision++;
  }
  return !r->failed;
}
static bool linked_key(dg_runtime_t *parent, unsigned code, unsigned character, bool down) {
  bool routed = false;
  for (dg_linked_child_t *c = parent->linked_children; c; c = c->next) {
    dg_runtime_t *child = c->runtime;
    bool held = (child->keys_down[code >> 5] & (UINT32_C(1) << (code & 31))) != 0;
    // Release follows the instance that received the press even after focus
    // moves. Physical key state remains visible in each enclosing runtime.
    if ((down && c->channel == parent->linked_focus && (child->linked_flags & 16)) || (!down && held)) {
      dg_key(child, code, character, down);
      routed = true;
    }
  }
  return routed;
}
static bool linked_get(dg_runtime_t *d, const char *name, lv_t owner, lv_t *out) {
  if (lv_type(owner) != LV_SPRITE) return false;
  unsigned n = (unsigned)lv_id(owner);
  const dg_runtime_t *child = dg_linked_movie(d, n);
  const dg_member_t *m = n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
  if (!m || m->type != 9) return false;
  unsigned ln = lv_name_id(d->values, name);
  if (ln == LN_FRAME) *out = lv_num(child ? child->frame : 1);
  else if (ln == LN_PLAYING) *out = lv_num(child && !child->linked_paused && !d->sprites[n].film_paused);
  else return false;
  return true;
}
static bool linked_call(dg_runtime_t *d, const char *name, unsigned argc, const lv_t *a, lv_t *out) {
  if (!argc || lv_type(a[0]) != LV_SPRITE) return false;
  unsigned n = (unsigned)lv_id(a[0]);
  const dg_member_t *m = n < DG_SPRITES ? dg_member(d, d->sprites[n].value.member) : NULL;
  if (!m || m->type != 9) return false;
  unsigned ln = lv_name_id(d->values, name);
  bool seek = ln == LN_GOTOFRAME || ln == LN_SPRITE_GO || ln == LN_GOTOANDPLAY || ln == LN_GOTOANDSTOP;
  if (!seek && ln != LN_PLAY && ln != LN_STOP && ln != LN_REWIND) return false;
  if (argc != (seek ? 2u : 1u)) { lv_fail(d->values, "linked movie control arguments"); return true; }
  if (!linked_find(d, n) && !linked_create(d, n, m)) return true;
  dg_runtime_t *child = dg_linked_begin(d, n);
  if (seek || ln == LN_REWIND) {
    unsigned frame = ln == LN_REWIND ? 1 : frame_number(child, a[1]);
    child->values->depth = 0; child->values->yielded = false;
    if (frame) dg_seek(child, frame);
  }
  if (ln == LN_STOP || ln == LN_GOTOANDSTOP) child->linked_paused = true;
  if (ln == LN_PLAY || ln == LN_GOTOANDPLAY) child->linked_paused = false;
  d->sprites[n].film_paused = child->linked_paused;
  dg_update_stage(child); dg_linked_end(d); d->visual_revision++;
  *out = lv_num(1); return true;
}
