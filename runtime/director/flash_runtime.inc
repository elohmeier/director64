// Independent Flash display-list instances. Format facts follow Adobe SWF19;
// this implementation does not reuse another player's implementation code.
#include "avm1_native.h"
#include "flash_bridge.h"
#include "flash_runtime.h"
#define FLASH_INSTANCE_LIMIT 2048u
#define FLASH_QUEUE_LIMIT 1024u
#define FLASH_ACTION_LIMIT 4096u
#define FLASH_VOICE_LIMIT 64u

typedef struct dg_flash_movie dg_flash_movie_t;
typedef struct {
  dg_flash_instance_t *instance;
  const uint8_t *code;
  unsigned length;
  bool constructor;
} flash_action_job_t;
typedef struct {
  dg_flash_audio_t value;
  uint32_t tick;
} flash_voice_t;
static uint32_t flash_voice_serial;
struct dg_flash_instance {
  dg_flash_instance_t *next, *parent;
  dg_flash_movie_t *movie;
  const dg_film_layer_t *placement;
  const dg_flash_clip_t *clip;
  const dg_flash_button_t *button;
  const dg_flash_field_t *field;
  lv_root_span_t root;
  lv_t object, constructor;
  uint32_t id, generation, visit;
  unsigned depth, frame, overridden;
  float matrix[6], bounds[4], alpha, scale_sign[2];
  int16_t cxform[8];
  dg_flash_path_t *paths;
  dg_flash_point_t *points;
  unsigned path_count, point_count, path_capacity, point_capacity;
  dg_flash_path_t pen;
  float pen_x, pen_y;
  bool path_open;
  dg_flash_field_t *owned_field;
  dg_text_style_t *owned_style;
  char *name;
  bool alive, playing, visible, dynamic, color_override;
};
struct dg_flash_movie {
  dg_flash_movie_t *next;
  dg_runtime_t *director;
  const dg_member_t *member;
  unsigned sprite, count, visit, queue_first, queue_count, last_tick,
      pending_frame;
  dg_avm1_t *vm;
  dg_avm1_library_t *library;
  dg_flash_instance_t *instances, *root, *drag, *hover, *pressed;
  bool *initialized;
  flash_action_job_t queue[FLASH_QUEUE_LIMIT];
  flash_voice_t voices[FLASH_VOICE_LIMIT];
  float drag_dx, drag_dy, drag_bounds[4];
  float mouse_x, mouse_y;
  bool busy, drag_bounded, mouse_down, pending_enter;
};
struct dg_flash_state {
  dg_flash_movie_t *movies;
  uint32_t serial;
};

static bool flash_host_get(void *, dg_avm1_t *, lv_t, const char *, lv_t *);
static bool flash_host_set(void *, dg_avm1_t *, lv_t, const char *, lv_t);
static bool flash_host_call(void *, dg_avm1_t *, const char *, lv_t, unsigned,
                            const lv_t *, lv_t *);
static lv_t flash_host_target(void *, dg_avm1_t *, lv_t, const char *);
static void flash_sync_children(dg_flash_instance_t *);
static void flash_enter(dg_flash_instance_t *, unsigned, bool);
static void flash_remove(dg_flash_instance_t *);
static void flash_sound_events(dg_flash_instance_t *, unsigned);
static bool flash_drawing(dg_flash_instance_t *, const char *, unsigned,
                          const lv_t *, lv_t *);
static bool flash_display_hit(dg_flash_instance_t *, float, float, unsigned,
                              bool);

static void flash_host_trace(void *context, const char *text) {
  dg_flash_movie_t *movie = context;
  dg_runtime_t *director = movie->director;
  if (director->platform.trace)
    director->platform.trace(director->context, text);
}

static void flash_root_unlink(lv_runtime_t *r, lv_root_span_t *span) {
  lv_root_span_t **at = &r->external_roots;
  while (*at && *at != span)
    at = &(*at)->next;
  if (*at)
    *at = span->next;
}
static const dg_flash_clip_t *flash_clip(const dg_member_t *m, unsigned id) {
  for (unsigned i = 0; i < m->flash_clip_count; i++)
    if (m->flash_clips[i].id == id)
      return &m->flash_clips[i];
  return NULL;
}
static const dg_flash_button_t *flash_button(const dg_member_t *m,
                                             unsigned id) {
  for (unsigned i = 0; i < m->flash_button_count; i++)
    if (m->flash_buttons[i].id == id)
      return &m->flash_buttons[i];
  return NULL;
}
static const dg_flash_field_t *flash_field(const dg_member_t *m, unsigned id) {
  for (unsigned i = 0; i < m->flash_field_count; i++)
    if (m->flash_fields[i].id == id)
      return &m->flash_fields[i];
  return NULL;
}
static const dg_film_frame_t *flash_frame(const dg_flash_instance_t *i) {
  if (i->button)
    return &i->button->states[i->frame < 3 ? i->frame : 0];
  if (!i->clip || !i->clip->frame_count)
    return NULL;
  const dg_film_frame_t *frames =
      i->clip->frames ? i->clip->frames : i->movie->member->film_frames;
  return frames ? &frames[i->frame % i->clip->frame_count] : NULL;
}
static void flash_changed(dg_flash_instance_t *i) {
  dg_sprite_changed(i->movie->director, i->movie->sprite);
}
static void flash_queue(dg_flash_instance_t *i,
                        const dg_flash_action_t *action) {
  dg_flash_movie_t *m = i->movie;
  if (!action->length)
    return;
  if (!action->code || m->queue_count == FLASH_QUEUE_LIMIT) {
    lv_fail(m->director->values,
            "Flash action queue capacity or missing bytecode");
    return;
  }
  unsigned at = (m->queue_first + m->queue_count++) % FLASH_QUEUE_LIMIT;
  m->queue[at] = (flash_action_job_t){
      .instance = i, .code = action->code, .length = action->length};
}
static void flash_actions(dg_flash_instance_t *i, unsigned frame,
                          uint32_t event) {
  if (i->clip)
    for (unsigned n = 0; n < i->clip->action_count; n++) {
      const dg_flash_action_t *a = &i->clip->actions[n];
      if (event ? (a->event & event) != 0 : (!a->event && a->frame == frame))
        flash_queue(i, a);
    }
  if (event && i->placement)
    for (unsigned n = 0; n < i->placement->action_count; n++) {
      const dg_flash_action_t *a = &i->placement->actions[n];
      if (a->event & event)
        flash_queue(i, a);
    }
}
static bool flash_run(dg_flash_movie_t *m, unsigned budget) {
  unsigned actions = 0;
  while (!m->director->values->failed) {
    if (m->busy) {
      if (!budget)
        return true;
      unsigned slice = budget < 256 ? budget : 256;
      budget -= slice;
      dg_avm1_status_t status = dg_avm1_run(m->vm, slice);
      if (status == DG_AVM1_BUSY)
        continue;
      m->busy = false;
      if (status == DG_AVM1_ERROR)
        return false;
    }
    if (!m->queue_count)
      return true;
    if (!budget)
      return true;
    if (++actions > FLASH_ACTION_LIMIT) {
      lv_fail(m->director->values, "Flash frame action cycle exceeded budget");
      return false;
    }
    flash_action_job_t job = m->queue[m->queue_first];
    m->queue_first = (m->queue_first + 1) % FLASH_QUEUE_LIMIT;
    m->queue_count--;
    if (job.constructor) {
      lv_t result;
      if (!dg_avm1_invoke(m->vm, job.instance->constructor,
                          job.instance->object, 0, NULL, budget, &result))
        return false;
      budget = 0;
      continue;
    }
    // Unload actions keep their receiver alive after display-list removal.
    if (!dg_avm1_start(m->vm, job.instance->object, job.code, job.length))
      return false;
    m->busy = true;
  }
  return false;
}
static dg_flash_instance_t *flash_find(dg_flash_movie_t *m, lv_t object) {
  for (dg_flash_instance_t *i = m->instances; i; i = i->next)
    if (i->object == object)
      return i;
  return NULL;
}
dg_flash_instance_t *dg_flash_object_instance(dg_runtime_t *d, lv_t object) {
  if (d->flash_state)
    for (dg_flash_movie_t *m = d->flash_state->movies; m; m = m->next) {
      dg_flash_instance_t *i = flash_find(m, object);
      if (i)
        return i;
    }
  return NULL;
}
dg_avm1_t *dg_flash_vm_for_object(dg_runtime_t *d, lv_t object) {
  if (d->flash_state)
    for (dg_flash_movie_t *m = d->flash_state->movies; m; m = m->next)
      if (dg_avm1_owns(m->vm, object))
        return m->vm;
  return NULL;
}
dg_flash_instance_t *dg_flash_root(dg_runtime_t *d, unsigned sprite) {
  if (d->flash_state)
    for (dg_flash_movie_t *m = d->flash_state->movies; m; m = m->next)
      if (m->sprite == sprite)
        return m->root;
  return NULL;
}
dg_flash_instance_t *dg_flash_child(dg_flash_instance_t *parent,
                                    unsigned depth) {
  if (!parent)
    return NULL;
  for (dg_flash_instance_t *i = parent->movie->instances; i; i = i->next)
    if (i->alive && i->parent == parent && i->depth == depth)
      return i;
  return NULL;
}
dg_flash_instance_t *dg_flash_parent(dg_flash_instance_t *i) {
  return i ? i->parent : NULL;
}
dg_avm1_t *dg_flash_vm(dg_flash_instance_t *i) {
  return i ? i->movie->vm : NULL;
}
lv_t dg_flash_object(dg_flash_instance_t *i) { return i ? i->object : 0; }
const dg_film_layer_t *dg_flash_placement(const dg_flash_instance_t *i) {
  return i ? i->placement : NULL;
}
bool dg_flash_view(const dg_flash_instance_t *i, dg_flash_view_t *out) {
  if (!i || !i->alive)
    return false;
  *out = (dg_flash_view_t){.member = i->movie->member,
                           .frame = flash_frame(i),
                           .field = i->field,
                           .object = i->object,
                           .sprite = i->movie->sprite,
                           .frame_number = i->frame + 1,
                           .frame_count = i->clip ? i->clip->frame_count : 1,
                           .id = i->id,
                           .alpha = i->alpha,
                           .visible = i->visible,
                           .playing = i->playing};
  memcpy(out->matrix, i->matrix, sizeof(out->matrix));
  memcpy(out->bounds, i->bounds, sizeof(out->bounds));
  out->has_cxform = i->color_override;
  if (i->color_override)
    memcpy(out->cxform, i->cxform, sizeof(out->cxform));
  return true;
}
static void flash_matrix(dg_flash_instance_t *i, const dg_film_layer_t *p) {
  float authored[6] = {1, 0, 0, 1, 0, 0};
  if (p && p->affine)
    for (unsigned n = 0; n < 6; n++)
      authored[n] = p->matrix[n] / 65536.0f;
  else if (p) {
    authored[4] = p->origin_x / 65536.0f;
    authored[5] = p->origin_y / 65536.0f;
  }
  if (!(i->overridden & 1))
    i->matrix[4] = authored[4];
  if (!(i->overridden & 2))
    i->matrix[5] = authored[5];
  if (!(i->overridden & 4)) {
    memcpy(i->matrix, authored, 4 * sizeof(float));
    i->scale_sign[0] = i->scale_sign[1] = 1;
  }
}
static dg_flash_instance_t *flash_new(dg_flash_movie_t *m,
                                      dg_flash_instance_t *parent,
                                      const dg_film_layer_t *p,
                                      unsigned clip_id, const char *name,
                                      unsigned depth, bool dynamic) {
  if (m->count >= FLASH_INSTANCE_LIMIT) {
    lv_fail(m->director->values, "Flash instance capacity");
    return NULL;
  }
  dg_flash_instance_t *i = calloc(1, sizeof(*i));
  if (!i) {
    lv_fail(m->director->values, "Flash instance allocation");
    return NULL;
  }
  i->movie = m;
  i->parent = parent;
  i->placement = p;
  i->depth = depth;
  i->dynamic = dynamic;
  i->alive = i->playing = i->visible = true;
  i->alpha = 100;
  i->generation = p ? p->placement_generation : 0;
  i->id = ++m->director->flash_state->serial;
  i->clip = (!parent || clip_id) ? flash_clip(m->member, clip_id) : NULL;
  i->button = p && p->button_id ? flash_button(m->member, p->button_id) : NULL;
  i->field = p && p->field_id ? flash_field(m->member, p->field_id) : NULL;
  if ((clip_id && !i->clip) || (p && p->button_id && !i->button) ||
      (p && p->field_id && !i->field)) {
    free(i);
    lv_fail(m->director->values, "Flash character definition missing");
    return NULL;
  }
  const int32_t *bounds = i->clip     ? i->clip->local_bounds
                          : i->button ? i->button->local_bounds
                          : i->field  ? i->field->local_bounds
                                      : NULL;
  if (bounds)
    for (unsigned n = 0; n < 4; n++)
      i->bounds[n] = bounds[n] / 65536.0f;
  else if (p) {
    i->bounds[0] = p->value.x;
    i->bounds[1] = p->value.y;
    i->bounds[2] = p->value.width;
    i->bounds[3] = p->value.height;
  }
  size_t len = strlen(name ? name : "");
  i->name = malloc(len + 1);
  if (!i->name) {
    free(i);
    lv_fail(m->director->values, "Flash instance name allocation");
    return NULL;
  }
  memcpy(i->name, name ? name : "", len + 1);
  flash_matrix(i, p);
  lv_t prototype = dg_avm1_library_prototype(
      m->library, i->field ? "TextField" : "MovieClip");
  if (i->clip && parent)
    for (unsigned n = 0; n < m->member->flash_export_count; n++)
      if (m->member->flash_exports[n].id == i->clip->id) {
        lv_t constructor =
            dg_avm1_library_class(m->library, m->member->flash_exports[n].name);
        if (dg_avm1_callable(m->vm, constructor)) {
          i->constructor = constructor;
          lv_t candidate = dg_avm1_get(m->vm, constructor, "prototype");
          if (lv_type(candidate) == LV_PROPLIST)
            prototype = candidate;
        }
      }
  i->object = dg_avm1_object(m->vm, prototype);
  i->root = (lv_root_span_t){.next = m->director->values->external_roots,
                             .values = &i->object,
                             .count = 2};
  m->director->values->external_roots = &i->root;
  i->next = m->instances;
  m->instances = i;
  m->count++;
  if (i->field)
    dg_avm1_own_set(m->vm, i->object, "text",
                    lv_text(m->director->values,
                            i->field->text ? i->field->text : "", false));
  if (!parent)
    m->root = i;
  if (!parent)
    return i;
  flash_actions(i, 0, 0x0200u); // CLIPEVENTFLAGS byte 1 bit 1: initialize
  flash_sync_children(i);
  flash_actions(i, 0, 0x40000u); // construct
  if (i->constructor) {
    if (m->queue_count == FLASH_QUEUE_LIMIT)
      lv_fail(m->director->values, "Flash constructor queue capacity");
    else {
      unsigned at = (m->queue_first + m->queue_count++) % FLASH_QUEUE_LIMIT;
      m->queue[at] = (flash_action_job_t){.instance = i, .constructor = true};
    }
  }
  flash_sound_events(i, 1);
  flash_actions(i, 1, 0);
  flash_actions(i, 0, 1); // frame1, then load
  return i;
}
static void flash_remove(dg_flash_instance_t *i) {
  if (!i || !i->alive)
    return;
  for (dg_flash_instance_t *c = i->movie->instances; c; c = c->next)
    if (c->alive && c->parent == i)
      flash_remove(c);
  flash_actions(i, 0, 4);
  i->alive = false;
  if (i->movie->drag == i)
    i->movie->drag = NULL;
  flash_changed(i);
}
static void flash_sync_children(dg_flash_instance_t *parent) {
  const dg_film_frame_t *frame = flash_frame(parent);
  unsigned serial = ++parent->movie->visit;
  if (frame)
    for (unsigned n = 0; n < frame->count; n++) {
      const dg_film_layer_t *p = &frame->layers[n];
      if (!p->clip_id && !p->button_id && !p->field_id &&
          (!p->name || !*p->name))
        continue;
      dg_flash_instance_t *i = dg_flash_child(parent, p->channel);
      bool same = i && !i->dynamic &&
                  i->generation == p->placement_generation &&
                  ((!p->clip_id && !i->clip) ||
                   (i->clip && i->clip->id == p->clip_id)) &&
                  ((!p->button_id && !i->button) ||
                   (i->button && i->button->id == p->button_id)) &&
                  ((!p->field_id && !i->field) ||
                   (i->field && i->field->id == p->field_id));
      if (i && i->dynamic) {
        i->visit = serial;
        continue;
      }
      if (i && !same) {
        flash_remove(i);
        i = NULL;
      }
      if (!i)
        i = flash_new(parent->movie, parent, p, p->clip_id, p->name, p->channel,
                      false);
      if (i) {
        i->placement = p;
        i->visit = serial;
        flash_matrix(i, p);
      }
    }
  for (dg_flash_instance_t *i = parent->movie->instances; i; i = i->next)
    if (i->alive && i->parent == parent && !i->dynamic && i->visit != serial)
      flash_remove(i);
}
static void flash_enter(dg_flash_instance_t *i, unsigned frame, bool actions) {
  if (!i->clip || !i->clip->frame_count)
    return;
  frame %= i->clip->frame_count;
  if (!i->parent) {
    dg_flash_movie_t *m = i->movie;
    bool queued = false;
    for (unsigned n = 0; n < m->member->flash_initializer_count; n++) {
      const dg_flash_initializer_t *init = &m->member->flash_initializers[n];
      if (!m->initialized[n] && init->action.frame <= frame + 1) {
        for (unsigned k = 0; k < m->member->flash_initializer_count; k++)
          if (m->member->flash_initializers[k].clip_id == init->clip_id)
            m->initialized[k] = true;
        flash_queue(i, &init->action);
        queued = true;
      }
    }
    if (queued) {
      m->pending_enter = true;
      m->pending_frame = frame;
      if (m->busy || !flash_run(m, 100000) || m->busy || m->queue_count)
        return;
      m->pending_enter = false;
    }
  }
  bool changed = i->frame != frame;
  i->frame = frame;
  flash_sync_children(i);
  if (changed)
    flash_changed(i);
  if (!i->parent)
    i->movie->director->sprites[i->movie->sprite].film_frame = frame;
  if (actions) {
    flash_sound_events(i, frame + 1);
    flash_actions(i, frame + 1, 0);
  }
}
static unsigned flash_frame_number(dg_flash_instance_t *i, lv_t value) {
  if (lv_type(value) == LV_STRING && i->clip) {
    const char *label = lv_cstr(i->movie->director->values, value);
    for (unsigned n = 0; n < i->clip->label_count; n++)
      if (!strcmp(label, i->clip->labels[n].name))
        return i->clip->labels[n].frame ? i->clip->labels[n].frame - 1 : 0;
  }
  double number = dg_avm1_number(i->movie->vm, value);
  unsigned max = i->clip ? i->clip->frame_count : 1;
  if (!isfinite(number) || number < 1)
    return 0;
  return number > max ? max - 1 : (unsigned)number - 1;
}
static void flash_seek_instance(dg_flash_instance_t *i, unsigned frame,
                                bool play) {
  i->playing = play;
  if (!i->parent)
    i->movie->director->sprites[i->movie->sprite].film_paused = !play;
  flash_enter(i, frame, true);
  flash_changed(i);
}
void dg_flash_seek(dg_runtime_t *d, unsigned sprite, unsigned frame,
                   bool play) {
  dg_flash_instance_t *i = dg_flash_root(d, sprite);
  if (i)
    flash_seek_instance(i, frame, play);
}
void dg_flash_play(dg_runtime_t *d, unsigned sprite, bool play) {
  dg_flash_instance_t *i = dg_flash_root(d, sprite);
  if (i) {
    i->playing = play;
    d->sprites[sprite].film_paused = !play;
  }
}
static bool flash_name_equal(const char *a, const char *b, bool fold) {
  while (*a && *b &&
         ((fold ? tolower((unsigned char)*a) : (unsigned char)*a) ==
          (fold ? tolower((unsigned char)*b) : (unsigned char)*b))) {
    a++;
    b++;
  }
  return !*a && !*b;
}
static dg_flash_instance_t *flash_named(dg_flash_instance_t *parent,
                                        const char *name) {
  if (!parent)
    return NULL;
  for (dg_flash_instance_t *i = parent->movie->instances; i; i = i->next)
    if (i->alive && i->parent == parent &&
        flash_name_equal(i->name, name,
                         parent->movie->member->flash_version <= 6))
      return i;
  return NULL;
}
dg_flash_instance_t *dg_flash_named(dg_flash_instance_t *parent,
                                    const char *name, bool fold) {
  if (!fold)
    return flash_named(parent, name);
  if (parent)
    for (dg_flash_instance_t *i = parent->movie->instances; i; i = i->next)
      if (i->alive && i->parent == parent) {
        const char *a = i->name, *b = name;
        while (*a && *b &&
               tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
          a++;
          b++;
        }
        if (!*a && !*b)
          return i;
      }
  return NULL;
}
static lv_t flash_host_target(void *ctx, dg_avm1_t *vm, lv_t object,
                              const char *path) {
  (void)vm;
  dg_flash_movie_t *m = ctx;
  dg_flash_instance_t *i = flash_find(m, object);
  if (!i)
    i = m->root;
  if (*path == '/') {
    i = m->root;
    path++;
  }
  char part[128];
  while (*path && i) {
    unsigned n = 0;
    while (*path && *path != '/' && *path != '.' && *path != ':') {
      if (n + 1 == sizeof(part)) {
        lv_fail(m->director->values, "Flash target component too long");
        return 0;
      }
      part[n++] = *path++;
    }
    part[n] = 0;
    if (*path)
      path++;
    if (!n)
      continue;
    if (!strcmp(part, "_root") || !strcmp(part, "_level0"))
      i = m->root;
    else if (!strcmp(part, "_parent"))
      i = i->parent;
    else if (strcmp(part, "this"))
      i = flash_named(i, part);
  }
  return i ? i->object : 0;
}
lv_t dg_flash_target(dg_flash_instance_t *instance, const char *path) {
  return instance ? flash_host_target(instance->movie, instance->movie->vm,
                                      instance->object, path)
                  : 0;
}
static void flash_world(const dg_flash_instance_t *i, float out[6]) {
  out[0] = out[3] = 1;
  out[1] = out[2] = out[4] = out[5] = 0;
  while (i && i->parent) {
    const float *m = i->matrix;
    float old[6];
    memcpy(old, out, sizeof(old));
    out[0] = m[0] * old[0] + m[2] * old[1];
    out[1] = m[1] * old[0] + m[3] * old[1];
    out[2] = m[0] * old[2] + m[2] * old[3];
    out[3] = m[1] * old[2] + m[3] * old[3];
    out[4] = m[0] * old[4] + m[2] * old[5] + m[4];
    out[5] = m[1] * old[4] + m[3] * old[5] + m[5];
    i = i->parent;
  }
}
static bool flash_inverse(const float m[6], float *x, float *y) {
  float det = m[0] * m[3] - m[1] * m[2];
  if (fabsf(det) < 0.0000001f)
    return false;
  float dx = *x - m[4], dy = *y - m[5];
  *x = (m[3] * dx - m[2] * dy) / det;
  *y = (-m[1] * dx + m[0] * dy) / det;
  return true;
}
static bool flash_point_local(dg_flash_instance_t *i, float *x, float *y) {
  float matrix[6];
  flash_world(i, matrix);
  return flash_inverse(matrix, x, y);
}
static void flash_point_global(dg_flash_instance_t *i, float *x, float *y) {
  float m[6];
  flash_world(i, m);
  float xx = *x, yy = *y;
  *x = m[0] * xx + m[2] * yy + m[4];
  *y = m[1] * xx + m[3] * yy + m[5];
}
static void flash_bounds_add(float bounds[4], bool *any, const float box[4],
                             const float matrix[6]) {
  for (unsigned corner = 0; corner < 4; corner++) {
    float x = box[0] + ((corner == 1 || corner == 2) ? box[2] : 0),
          y = box[1] + ((corner >= 2) ? box[3] : 0);
    float px = matrix[0] * x + matrix[2] * y + matrix[4],
          py = matrix[1] * x + matrix[3] * y + matrix[5];
    if (!*any) {
      bounds[0] = bounds[2] = px;
      bounds[1] = bounds[3] = py;
      *any = true;
    } else {
      bounds[0] = fminf(bounds[0], px);
      bounds[1] = fminf(bounds[1], py);
      bounds[2] = fmaxf(bounds[2], px);
      bounds[3] = fmaxf(bounds[3], py);
    }
  }
}
static bool flash_local_bounds(dg_flash_instance_t *i, float bounds[4],
                               unsigned nesting) {
  if (nesting >= 32) {
    lv_fail(i->movie->director->values, "Flash bounds nesting exceeded");
    return false;
  }
  bool any = false;
  float box[4], identity[6] = {1, 0, 0, 1, 0, 0};
  if (i->field || (i->placement && i->placement->asset))
    flash_bounds_add(bounds, &any, i->bounds, identity);
  for (unsigned path = 0; path < i->path_count; path++) {
    const dg_flash_path_t *p = &i->paths[path];
    float margin = p->stroked ? fmaxf(p->line_width, 1) / 2 : 0;
    if (!p->stroked && !p->filled)
      continue;
    for (unsigned point = 0; point < p->count; point++) {
      dg_flash_point_t xy = i->points[p->first + point];
      float dot[4] = {xy.x - margin, xy.y - margin, margin * 2, margin * 2};
      flash_bounds_add(bounds, &any, dot, identity);
    }
  }
  unsigned count = dg_flash_layer_count(i);
  for (unsigned n = 0; n < count; n++) {
    dg_flash_layer_view_t layer;
    if (!dg_flash_layer_at(i, n, &layer))
      continue;
    const dg_film_layer_t *p = layer.layer;
    if (p && p->clip_depth)
      continue;
    if (layer.instance) {
      if (flash_local_bounds(layer.instance, box, nesting + 1))
        flash_bounds_add(bounds, &any, box, layer.instance->matrix);
    } else if (p && p->asset) {
      float matrix[6] = {1, 0, 0, 1, 0, 0};
      if (p->affine)
        for (unsigned k = 0; k < 6; k++)
          matrix[k] = p->matrix[k] / 65536.0f;
      box[0] = p->value.x;
      box[1] = p->value.y;
      box[2] = p->value.width;
      box[3] = p->value.height;
      flash_bounds_add(bounds, &any, box, matrix);
    }
  }
  if (any) {
    bounds[2] -= bounds[0];
    bounds[3] -= bounds[1];
  } else
    memset(bounds, 0, 4 * sizeof(*bounds));
  return any;
}
static void flash_bounds_in(dg_flash_instance_t *i, dg_flash_instance_t *target,
                            float bounds[4]) {
  float local[4];
  flash_local_bounds(i, local, 0);
  float left = local[0], top = local[1], right = left + local[2],
        bottom = top + local[3];
  float x[4] = {left, right, right, left}, y[4] = {top, top, bottom, bottom};
  for (unsigned n = 0; n < 4; n++) {
    flash_point_global(i, &x[n], &y[n]);
    if (target)
      flash_point_local(target, &x[n], &y[n]);
    if (!n) {
      bounds[0] = bounds[2] = x[n];
      bounds[1] = bounds[3] = y[n];
    } else {
      bounds[0] = fminf(bounds[0], x[n]);
      bounds[1] = fminf(bounds[1], y[n]);
      bounds[2] = fmaxf(bounds[2], x[n]);
      bounds[3] = fmaxf(bounds[3], y[n]);
    }
  }
}
static bool flash_host_get(void *ctx, dg_avm1_t *vm, lv_t object,
                           const char *key, lv_t *out) {
  dg_flash_movie_t *m = ctx;
  dg_flash_instance_t *i = flash_find(m, object);
  if (!i)
    return false;
  char folded[128];
  if (m->member->flash_version <= 6 && key[0] == '_' &&
      strlen(key) < sizeof(folded)) {
    unsigned n = 0;
    do {
      folded[n] = (char)tolower((unsigned char)key[n]);
    } while (key[n++]);
    key = folded;
  }
  if (!i->alive) {
    *out = 0;
    return true;
  }
  dg_flash_instance_t *child = flash_named(i, key);
  if (child) {
    *out = child->object;
    return true;
  }
  if (!strcmp(key, "_parent")) {
    *out = i->parent ? i->parent->object : 0;
    return true;
  }
  if (!strcmp(key, "_root") || !strcmp(key, "_level0")) {
    *out = m->root->object;
    return true;
  }
  if (!strcmp(key, "_name")) {
    *out = lv_text(m->director->values, i->name, false);
    return true;
  }
  if (!strcmp(key, "_currentframe")) {
    *out = lv_num(i->frame + 1);
    return true;
  }
  if (!strcmp(key, "_totalframes") || !strcmp(key, "_framesloaded")) {
    *out = lv_num(i->clip ? i->clip->frame_count : 1);
    return true;
  }
  if (!strcmp(key, "_visible")) {
    *out = dg_avm1_boolean(i->visible);
    return true;
  }
  if (i->field) {
    if (dg_avm1_own_get(vm, object, key, out))
      return true;
    if (!i->field->style && (!strcmp(key, "size") || !strcmp(key, "font") ||
                             !strcmp(key, "textColor"))) {
      lv_fail(m->director->values,
              "Flash text field is missing its font descriptor");
      *out = 0;
      return true;
    }
    if (!strcmp(key, "size")) {
      *out = lv_num(i->field->style->size);
      return true;
    }
    if (!strcmp(key, "font")) {
      *out = lv_text(m->director->values, i->field->style->font_name, false);
      return true;
    }
    if (!strcmp(key, "textColor")) {
      *out = lv_num(i->field->style->color);
      return true;
    }
    if (!strcmp(key, "wordWrap")) {
      *out = dg_avm1_boolean(i->field->word_wrap);
      return true;
    }
    if (!strcmp(key, "multiline")) {
      *out = dg_avm1_boolean(i->field->multiline);
      return true;
    }
    if (!strcmp(key, "embedFonts")) {
      *out = dg_avm1_boolean(i->field->embed_fonts);
      return true;
    }
  }
  float number;
  if (!strcmp(key, "_x"))
    number = i->matrix[4];
  else if (!strcmp(key, "_y"))
    number = i->matrix[5];
  else if (!strcmp(key, "_alpha"))
    number = i->alpha;
  else if (!strcmp(key, "_xmouse") || !strcmp(key, "_ymouse")) {
    float x = m->mouse_x, y = m->mouse_y;
    flash_point_local(i, &x, &y);
    number = !strcmp(key, "_xmouse") ? x : y;
  } else if (!strcmp(key, "_xscale"))
    number = hypotf(i->matrix[0], i->matrix[1]) * 100 * i->scale_sign[0];
  else if (!strcmp(key, "_yscale"))
    number = hypotf(i->matrix[2], i->matrix[3]) * 100 * i->scale_sign[1];
  else if (!strcmp(key, "_rotation"))
    number = atan2f(i->matrix[1] * i->scale_sign[0],
                    i->matrix[0] * i->scale_sign[0]) *
             180 / 3.14159265358979323846f;
  else if (!strcmp(key, "_width") || !strcmp(key, "_height")) {
    bool width = !strcmp(key, "_width");
    float box[4];
    flash_local_bounds(i, box, 0);
    number = i->field ? i->bounds[width ? 2 : 3]
             : width
                 ? fabsf(i->matrix[0]) * box[2] + fabsf(i->matrix[2]) * box[3]
                 : fabsf(i->matrix[1]) * box[2] + fabsf(i->matrix[3]) * box[3];
  } else {
    (void)vm;
    return false;
  }
  *out = lv_num(number);
  return true;
}
static bool flash_host_set(void *ctx, dg_avm1_t *vm, lv_t object,
                           const char *key, lv_t value) {
  dg_flash_movie_t *m = ctx;
  dg_flash_instance_t *i = flash_find(m, object);
  if (!i)
    return false;
  char folded[128];
  if (m->member->flash_version <= 6 && key[0] == '_' &&
      strlen(key) < sizeof(folded)) {
    unsigned n = 0;
    do {
      folded[n] = (char)tolower((unsigned char)key[n]);
    } while (key[n++]);
    key = folded;
  }
  if (!i->alive)
    return true;
  if (!strcmp(key, "_visible")) {
    i->visible = dg_avm1_truth(vm, value);
    flash_changed(i);
    return true;
  }
  if (!strcmp(key, "_name")) {
    lv_t text = dg_avm1_string(vm, value);
    const char *s = lv_cstr(m->director->values, text);
    char *name = malloc(strlen(s) + 1);
    if (!name) {
      lv_fail(m->director->values, "Flash name allocation");
      return true;
    }
    strcpy(name, s);
    free(i->name);
    i->name = name;
    return true;
  }
  bool geometry = !strcmp(key, "_x") || !strcmp(key, "_y") ||
                  !strcmp(key, "_alpha") || !strcmp(key, "_xscale") ||
                  !strcmp(key, "_yscale") || !strcmp(key, "_rotation") ||
                  !strcmp(key, "_width") || !strcmp(key, "_height");
  if (!geometry) {
    if (i->field && (!strcmp(key, "text") || !strcmp(key, "htmlText"))) {
      dg_avm1_own_set(vm, object, "text", dg_avm1_string(vm, value));
      flash_changed(i);
      return true;
    }
    if (i->field && (!strcmp(key, "embedFonts") || !strcmp(key, "wordWrap") ||
                     !strcmp(key, "multiline") || !strcmp(key, "textColor"))) {
      dg_avm1_own_set(vm, object, key, value);
      flash_changed(i);
      return true;
    }
    return false;
  }
  double number = dg_avm1_number(vm, value);
  if (!isfinite(number) || fabs(number) > 1000000) {
    lv_fail(m->director->values, "invalid Flash geometry");
    return true;
  }
  float n = (float)number;
#ifdef DIRECTOR64_FLASH_GEOMETRY_TRACE
  fprintf(stderr,
          "FLASH_SET %s %s=%.4f matrix=%.4f,%.4f,%.4f,%.4f bounds=%.4f,%.4f\n",
          i->name, key, n, i->matrix[0], i->matrix[1], i->matrix[2],
          i->matrix[3], i->bounds[2], i->bounds[3]);
#endif
  if (!strcmp(key, "_x")) {
    i->matrix[4] = n;
    i->overridden |= 1;
  } else if (!strcmp(key, "_y")) {
    i->matrix[5] = n;
    i->overridden |= 2;
  } else if (!strcmp(key, "_alpha"))
    i->alpha = fmaxf(0, fminf(100, n));
  else if (i->field && (!strcmp(key, "_width") || !strcmp(key, "_height")))
    i->bounds[!strcmp(key, "_width") ? 2 : 3] = fmaxf(0, n);
  else if (!strcmp(key, "_rotation")) {
    float delta = n * 3.14159265358979323846f / 180 -
                  atan2f(i->matrix[1] * i->scale_sign[0],
                         i->matrix[0] * i->scale_sign[0]);
    float c = cosf(delta), s = sinf(delta), a = i->matrix[0], b = i->matrix[1],
          cc = i->matrix[2], dd = i->matrix[3];
    i->matrix[0] = a * c - b * s;
    i->matrix[1] = a * s + b * c;
    i->matrix[2] = cc * c - dd * s;
    i->matrix[3] = cc * s + dd * c;
    i->overridden |= 4;
  } else {
    bool x = !strcmp(key, "_width") || !strcmp(key, "_xscale");
    unsigned at = x ? 0 : 2;
    float old = hypotf(i->matrix[at], i->matrix[at + 1]);
    if (strstr(key, "scale")) {
      float scale = n / 100;
      if (old) {
        float ratio = scale / (old * i->scale_sign[x ? 0 : 1]);
        i->matrix[at] *= ratio;
        i->matrix[at + 1] *= ratio;
      } else {
        i->matrix[at] = x ? scale : 0;
        i->matrix[at + 1] = x ? 0 : scale;
      }
      i->scale_sign[x ? 0 : 1] = scale < 0 ? -1 : 1;
    } else {
      // Original Adobe Flash32 source-free geometry probes: _width/_height
      // derive both column scales from the axis-aligned transformed bounds.
      // This also preserves Flash's unintuitive behavior at 90 degrees.
      float box[4];
      flash_local_bounds(i, box, 0);
      float unit[4];
      memcpy(unit, i->matrix, sizeof(unit));
      for (unsigned column = 0; column < 2; column++) {
        float length = hypotf(unit[column * 2], unit[column * 2 + 1]) *
                       i->scale_sign[column];
        if (length) {
          unit[column * 2] /= length;
          unit[column * 2 + 1] /= length;
        }
      }
      float uw = fabsf(unit[0]) * box[2] + fabsf(unit[2]) * box[3];
      float uh = fabsf(unit[1]) * box[2] + fabsf(unit[3]) * box[3];
      float width = fabsf(i->matrix[0]) * box[2] + fabsf(i->matrix[2]) * box[3];
      float height =
          fabsf(i->matrix[1]) * box[2] + fabsf(i->matrix[3]) * box[3];
      float sx = uw ? (x ? n : width) / uw : 0,
            sy = uh ? (x ? height : n) / uh : 0;
      i->matrix[0] = unit[0] * sx;
      i->matrix[1] = unit[1] * sx;
      i->matrix[2] = unit[2] * sy;
      i->matrix[3] = unit[3] * sy;
      i->scale_sign[0] = sx < 0 ? -1 : 1;
      i->scale_sign[1] = sy < 0 ? -1 : 1;
    }
    i->overridden |= 4;
  }
  flash_changed(i);
  return true;
}
static bool flash_host_call(void *ctx, dg_avm1_t *vm, const char *name,
                            lv_t receiver, unsigned argc, const lv_t *argv,
                            lv_t *out) {
  dg_flash_movie_t *m = ctx;
  dg_flash_instance_t *i = flash_find(m, receiver);
  const char *method = strchr(name, '.');
  method = method ? method + 1 : name;
  if (!strcmp(method, "stopAllSounds")) {
    if (m->director->flash_state)
      for (dg_flash_movie_t *movie = m->director->flash_state->movies; movie;
           movie = movie->next)
        for (unsigned n = 0; n < FLASH_VOICE_LIMIT; n++)
          movie->voices[n].value.playing = false;
    *out = 0;
    return true;
  }
  if (!strcmp(method, "getURL")) {
    if (argc) {
      const char *url =
          lv_cstr(m->director->values, dg_avm1_string(vm, argv[0]));
      if (!dg_flash_lingo_url(m->director, url)) {
        lv_fail(m->director->values, "unsupported Flash URL scheme");
        return true;
      }
    }
    *out = 0;
    return true;
  }
  if (!i)
    return false;
  *out = 0;
  if (!i->alive)
    return true;
  if (flash_drawing(i, method, argc, argv, out))
    return true;
  if (!strncmp(name, "Color.", 6)) {
    int16_t transform[8] = {256, 256, 256, 256, 0, 0, 0, 0};
    if (i->color_override)
      memcpy(transform, i->cxform, sizeof(transform));
    else if (i->placement && i->placement->has_cxform)
      memcpy(transform, i->placement->cxform, sizeof(transform));
    if (!strcmp(method, "setRGB")) {
      if (argc) {
        uint32_t color = (uint32_t)dg_avm1_number(vm, argv[0]);
        transform[0] = transform[1] = transform[2] = 0;
        transform[4] = (color >> 16) & 255;
        transform[5] = (color >> 8) & 255;
        transform[6] = color & 255;
      }
    } else if (!strcmp(method, "getRGB")) {
      *out =
          lv_num(((uint32_t)(transform[4] & 255) << 16) |
                 ((uint32_t)(transform[5] & 255) << 8) | (transform[6] & 255));
      return true;
    } else if (!strcmp(method, "setTransform")) {
      if (argc) {
        const char *keys[] = {"ra", "ga", "ba", "aa", "rb", "gb", "bb", "ab"};
        for (unsigned n = 0; n < 8; n++) {
          lv_t value = dg_avm1_get(vm, argv[0], keys[n]);
          if (value) {
            double number = dg_avm1_number(vm, value) * (n < 4 ? 2.56 : 1);
            if (isfinite(number))
              transform[n] = (int16_t)fmax(-32768, fmin(32767, number));
          }
        }
      }
    } else if (!strcmp(method, "getTransform")) {
      *out =
          dg_avm1_object(vm, dg_avm1_library_prototype(m->library, "Object"));
      const char *keys[] = {"ra", "ga", "ba", "aa", "rb", "gb", "bb", "ab"};
      for (unsigned n = 0; n < 8; n++)
        dg_avm1_own_set(vm, *out, keys[n],
                        lv_num(transform[n] / (n < 4 ? 2.56 : 1)));
      return true;
    } else
      return false;
    memcpy(i->cxform, transform, sizeof(transform));
    i->color_override = true;
    flash_changed(i);
    return true;
  }
  if (!strcmp(method, "play") || !strcmp(method, "stop")) {
    i->playing = !strcmp(method, "play");
    if (!i->parent)
      m->director->sprites[m->sprite].film_paused = !i->playing;
    return true;
  }
  if (!strcmp(method, "gotoAndPlay") || !strcmp(method, "gotoAndStop")) {
    if (argc)
      flash_seek_instance(i, flash_frame_number(i, argv[0]),
                          !strcmp(method, "gotoAndPlay"));
    return true;
  }
  if (!strcmp(method, "nextFrame") || !strcmp(method, "prevFrame")) {
    unsigned frame = i->frame;
    if (!strcmp(method, "nextFrame")) {
      if (i->clip && frame + 1 < i->clip->frame_count)
        frame++;
    } else if (frame)
      frame--;
    flash_seek_instance(i, frame, false);
    return true;
  }
  if (!strcmp(method, "getDepth")) {
    *out = lv_num((int)i->depth - 16384);
    return true;
  }
  if (!strcmp(method, "getNextHighestDepth")) {
    unsigned max = 16384;
    for (dg_flash_instance_t *c = m->instances; c; c = c->next)
      if (c->alive && c->parent == i && c->depth >= max)
        max = c->depth + 1;
    *out = lv_num(max - 16384);
    return true;
  }
  if (!strcmp(method, "getInstanceAtDepth")) {
    dg_flash_instance_t *c =
        argc
            ? dg_flash_child(i, (unsigned)(dg_avm1_number(vm, argv[0]) + 16384))
            : NULL;
    *out = c ? c->object : 0;
    return true;
  }
  if (!strcmp(method, "removeMovieClip")) {
    if (i->parent)
      flash_remove(i);
    return true;
  }
  if (!strcmp(method, "removeTextField")) {
    if (i->field && i->parent)
      flash_remove(i);
    return true;
  }
  if (!strcmp(method, "localToGlobal") || !strcmp(method, "globalToLocal")) {
    if (argc) {
      float x = (float)dg_avm1_number(vm, dg_avm1_get(vm, argv[0], "x"));
      float y = (float)dg_avm1_number(vm, dg_avm1_get(vm, argv[0], "y"));
      if (!strcmp(method, "localToGlobal"))
        flash_point_global(i, &x, &y);
      else
        flash_point_local(i, &x, &y);
      dg_avm1_set(vm, argv[0], "x", lv_num(x));
      dg_avm1_set(vm, argv[0], "y", lv_num(y));
    }
    return true;
  }
  if (!strcmp(method, "getBounds")) {
    dg_flash_instance_t *target = argc ? flash_find(m, argv[0]) : i;
    float bounds[4];
    flash_bounds_in(i, target, bounds);
    *out = dg_avm1_object(vm, dg_avm1_library_prototype(m->library, "Object"));
    const char *keys[] = {"xMin", "yMin", "xMax", "yMax"};
    for (unsigned n = 0; n < 4; n++)
      dg_avm1_own_set(vm, *out, keys[n], lv_num(bounds[n]));
    return true;
  }
  if (!strcmp(method, "hitTest")) {
    bool hit = false;
    float a[4];
    flash_bounds_in(i, NULL, a);
    if (argc >= 2) {
      float x = (float)dg_avm1_number(vm, argv[0]),
            y = (float)dg_avm1_number(vm, argv[1]);
      hit = x >= a[0] && y >= a[1] && x < a[2] && y < a[3];
      if (hit && argc > 2 && dg_avm1_truth(vm, argv[2]))
        hit = flash_point_local(i, &x, &y) &&
              flash_display_hit(i, x, y, 0, false);
    } else if (argc) {
      dg_flash_instance_t *other = flash_find(m, argv[0]);
      if (other) {
        float b[4];
        flash_bounds_in(other, NULL, b);
        hit = a[0] < b[2] && a[2] > b[0] && a[1] < b[3] && a[3] > b[1];
      }
    }
    *out = dg_avm1_boolean(hit);
    return true;
  }
  if (!strcmp(method, "swapDepths")) {
    if (argc && i->parent) {
      dg_flash_instance_t *other = flash_find(m, argv[0]);
      double depth =
          other ? (double)other->depth : dg_avm1_number(vm, argv[0]) + 16384;
      if (isfinite(depth) && depth >= 0 && depth <= 1064959 &&
          (!other || other->parent == i->parent)) {
        if (!other)
          other = dg_flash_child(i->parent, (unsigned)depth);
        if (other) {
          other->depth = i->depth;
          other->dynamic = true;
        }
        i->depth = (unsigned)depth;
        i->dynamic = true;
        flash_changed(i);
      }
    }
    return true;
  }
  if (!strcmp(method, "startDrag")) {
    m->drag = i;
    float x = m->mouse_x, y = m->mouse_y;
    if (i->parent)
      flash_point_local(i->parent, &x, &y);
    bool center = argc && dg_avm1_truth(vm, argv[0]);
    m->drag_dx = center ? 0 : i->matrix[4] - x;
    m->drag_dy = center ? 0 : i->matrix[5] - y;
    m->drag_bounded = argc >= 5;
    if (m->drag_bounded)
      for (unsigned n = 0; n < 4; n++)
        m->drag_bounds[n] = (float)dg_avm1_number(vm, argv[n + 1]);
    return true;
  }
  if (!strcmp(method, "stopDrag")) {
    m->drag = NULL;
    return true;
  }
  if (!strcmp(method, "getTextFormat") || !strcmp(method, "getNewTextFormat")) {
    *out =
        dg_avm1_object(vm, dg_avm1_library_prototype(m->library, "TextFormat"));
    const char *keys[] = {"font",        "size",      "color",  "bold",
                          "italic",      "underline", "align",  "leftMargin",
                          "rightMargin", "indent",    "leading"};
    for (unsigned n = 0; n < sizeof(keys) / sizeof(*keys); n++) {
      lv_t value = dg_avm1_get(
          vm, receiver, !strcmp(keys[n], "color") ? "textColor" : keys[n]);
      if (value)
        dg_avm1_own_set(vm, *out, keys[n], value);
    }
    return true;
  }
  if (!strcmp(method, "setTextFormat") || !strcmp(method, "setNewTextFormat")) {
    if (argc) {
      lv_t format = argv[argc - 1];
      const char *keys[] = {"font",        "size",      "color",  "bold",
                            "italic",      "underline", "align",  "leftMargin",
                            "rightMargin", "indent",    "leading"};
      for (unsigned n = 0; n < sizeof(keys) / sizeof(*keys); n++) {
        lv_t value = dg_avm1_get(vm, format, keys[n]);
        if (value)
          dg_avm1_own_set(vm, receiver,
                          !strcmp(keys[n], "color") ? "textColor" : keys[n],
                          value);
      }
      flash_changed(i);
    }
    return true;
  }
  // Adobe AS2 MovieClip reference: creation is at a parent-local position;
  // duplicates restart at frame one and initObject is available to
  // constructors.
  if (!strcmp(method, "createTextField")) {
    if (argc < 6)
      return true;
    double depth = dg_avm1_number(vm, argv[1]) + 16384, geometry[4];
    if (!isfinite(depth) || depth < 0 || depth > 1064959)
      return true;
    for (unsigned n = 0; n < 4; n++) {
      geometry[n] = dg_avm1_number(vm, argv[n + 2]);
      if (!isfinite(geometry[n]) || fabs(geometry[n]) > 32767)
        return true;
    }
    const char *child_name =
        lv_cstr(m->director->values, dg_avm1_string(vm, argv[0]));
    dg_flash_instance_t *old = dg_flash_child(i, (unsigned)depth);
    if (old)
      flash_remove(old);
    dg_flash_instance_t *child =
        flash_new(m, i, NULL, 0, child_name, (unsigned)depth, true);
    if (!child)
      return true;
    child->owned_field = calloc(1, sizeof(*child->owned_field));
    child->owned_style = calloc(1, sizeof(*child->owned_style));
    if (!child->owned_field || !child->owned_style) {
      lv_fail(m->director->values, "Flash text field allocation");
      return true;
    }
    // A dynamic field's default face must be resolved from the selected media
    // before rendering. Font 255 is deliberately not a substitute glyph atlas.
    *child->owned_style = (dg_text_style_t){.font_name = "Times New Roman",
                                            .font_id = 255,
                                            .size = 12,
                                            .ascent = 12,
                                            .line_height = 14};
    *child->owned_field = (dg_flash_field_t){
        .name = child->name, .text = "", .style = child->owned_style};
    child->field = child->owned_field;
    child->playing = false;
    child->matrix[4] = (float)geometry[0];
    child->matrix[5] = (float)geometry[1];
    child->bounds[2] = fmaxf(0, (float)geometry[2]);
    child->bounds[3] = fmaxf(0, (float)geometry[3]);
    dg_avm1_set(vm, child->object, "__proto__",
                dg_avm1_library_prototype(m->library, "TextField"));
    dg_avm1_own_set(vm, child->object, "text",
                    lv_text(m->director->values, "", false));
    dg_avm1_own_set(vm, child->object, "type",
                    lv_text(m->director->values, "dynamic", false));
    dg_avm1_own_set(vm, child->object, "selectable", dg_avm1_boolean(true));
    if (m->member->flash_version >= 8)
      *out = child->object;
    flash_changed(child);
    return true;
  }
  if (!strcmp(method, "duplicateMovieClip")) {
    if (argc < 2 || !i->parent || i->field || i->button)
      return true;
    double depth = dg_avm1_number(vm, argv[1]) + 16384;
    if (!isfinite(depth) || depth < 0 || depth > 1064959)
      return true;
    const char *child_name =
        lv_cstr(m->director->values, dg_avm1_string(vm, argv[0]));
    dg_flash_instance_t *old = dg_flash_child(i->parent, (unsigned)depth);
    if (old)
      flash_remove(old);
    dg_flash_instance_t *child =
        flash_new(m, i->parent, i->placement, i->clip ? i->clip->id : 0,
                  child_name, (unsigned)depth, true);
    if (!child)
      return true;
    memcpy(child->matrix, i->matrix, sizeof(child->matrix));
    memcpy(child->scale_sign, i->scale_sign, sizeof(child->scale_sign));
    child->overridden = 7;
    child->alpha = i->alpha;
    child->visible = i->visible;
    child->color_override = i->color_override;
    memcpy(child->cxform, i->cxform, sizeof(child->cxform));
    if (i->point_count) {
      child->points = malloc(i->point_count * sizeof(*child->points));
      child->paths = malloc(i->path_count * sizeof(*child->paths));
      if (!child->points || !child->paths) {
        lv_fail(m->director->values, "Flash duplicate graphics allocation");
        return true;
      }
      memcpy(child->points, i->points, i->point_count * sizeof(*child->points));
      memcpy(child->paths, i->paths, i->path_count * sizeof(*child->paths));
      child->point_capacity = child->point_count = i->point_count;
      child->path_capacity = child->path_count = i->path_count;
      child->pen = i->pen;
      child->pen_x = i->pen_x;
      child->pen_y = i->pen_y;
      child->path_open = i->path_open;
    }
    if (argc > 2 && lv_type(argv[2]) == LV_PROPLIST)
      for (unsigned n = 1; n <= lv_count(m->director->values, argv[2]); n++) {
        lv_t key = lv_at(m->director->values, argv[2], n * 2 - 1);
        if (lv_type(key) == LV_STRING || lv_type(key) == LV_SYMBOL)
          dg_avm1_set(vm, child->object, lv_cstr(m->director->values, key),
                      lv_at(m->director->values, argv[2], n * 2));
      }
    *out = child->object;
    flash_changed(child);
    return true;
  }
  if (!strcmp(method, "attachMovie") ||
      !strcmp(method, "createEmptyMovieClip")) {
    bool attach = !strcmp(method, "attachMovie");
    unsigned offset = attach ? 1 : 0;
    if (argc < offset + 2)
      return true;
    unsigned id = 0;
    if (attach) {
      const char *symbol =
          lv_cstr(m->director->values, dg_avm1_string(vm, argv[0]));
      for (unsigned n = 0; n < m->member->flash_export_count; n++)
        if (!strcmp(symbol, m->member->flash_exports[n].name))
          id = m->member->flash_exports[n].id;
      if (!id)
        return true;
    }
    const char *child_name =
        lv_cstr(m->director->values, dg_avm1_string(vm, argv[offset]));
    double depth = dg_avm1_number(vm, argv[offset + 1]) + 16384;
    if (!isfinite(depth) || depth < 0 || depth > 1064959)
      return true;
    dg_flash_instance_t *old = dg_flash_child(i, (unsigned)depth);
    if (old)
      flash_remove(old);
    dg_flash_instance_t *child =
        flash_new(m, i, NULL, id, child_name, (unsigned)depth, true);
    if (child) {
      *out = child->object;
      if (attach && argc > 3 && lv_type(argv[3]) == LV_PROPLIST)
        for (unsigned n = 1; n <= lv_count(m->director->values, argv[3]); n++) {
          lv_t key = lv_at(m->director->values, argv[3], n * 2 - 1);
          if (lv_type(key) == LV_STRING || lv_type(key) == LV_SYMBOL)
            dg_avm1_set(vm, child->object, lv_cstr(m->director->values, key),
                        lv_at(m->director->values, argv[3], n * 2));
        }
      flash_changed(child);
    }
    return true;
  }
  return false;
}
static void flash_movie_dispose(dg_flash_movie_t *m) {
  dg_avm1_library_dispose(m->library);
  dg_avm1_dispose(m->vm);
  dg_flash_instance_t *i = m->instances;
  while (i) {
    dg_flash_instance_t *next = i->next;
    flash_root_unlink(m->director->values, &i->root);
    free(i->name);
    free(i->paths);
    free(i->points);
    free(i->owned_field);
    free(i->owned_style);
    free(i);
    i = next;
  }
  free(m->initialized);
  free(m);
}
void dg_flash_dispose(dg_runtime_t *d) {
  if (!d->flash_state)
    return;
  dg_flash_movie_t *m = d->flash_state->movies;
  while (m) {
    dg_flash_movie_t *next = m->next;
    flash_movie_dispose(m);
    m = next;
  }
  free(d->flash_state);
  d->flash_state = NULL;
}
void dg_flash_sync(dg_runtime_t *d) {
  if (d->flash_state) {
    dg_flash_movie_t **at = &d->flash_state->movies;
    while (*at) {
      dg_flash_movie_t *m = *at;
      const dg_sprite_t *s = &d->sprites[m->sprite];
      if (!s->value.type || s->value.member != m->member->id) {
        *at = m->next;
        flash_movie_dispose(m);
      } else
        at = &m->next;
    }
  }
  for (unsigned channel = 1; channel < DG_SPRITES && !d->values->failed;
       channel++) {
    const dg_sprite_t *s = &d->sprites[channel];
    if (!s->value.type)
      continue;
    const dg_member_t *member = dg_member(d, s->value.member);
    if (!member || !member->flash_clip_count || !member->film_frames ||
        dg_flash_root(d, channel))
      continue;
    if (!d->flash_state)
      d->flash_state = calloc(1, sizeof(*d->flash_state));
    dg_flash_movie_t *m = calloc(1, sizeof(*m));
    if (!d->flash_state || !m) {
      free(m);
      lv_fail(d->values, "Flash runtime allocation");
      return;
    }
    m->director = d;
    m->member = member;
    m->sprite = channel;
    m->last_tick = UINT32_MAX;
    m->initialized = calloc(
        member->flash_initializer_count ? member->flash_initializer_count : 1,
        sizeof(*m->initialized));
    if (!m->initialized) {
      free(m);
      lv_fail(d->values, "Flash initialization table allocation");
      return;
    }
    dg_avm1_host_t host = {.get = flash_host_get,
                           .set = flash_host_set,
                           .call = flash_host_call,
                           .target = flash_host_target,
                           .trace = flash_host_trace};
    m->library = dg_avm1_library_create(host, m);
    m->vm =
        m->library
            ? dg_avm1_create(d->values,
                             member->flash_version ? member->flash_version : 8,
                             dg_avm1_library_host(), m->library)
            : NULL;
    if (!m->vm || !dg_avm1_library_bind(m->library, m->vm)) {
      flash_movie_dispose(m);
      return;
    }
    m->next = d->flash_state->movies;
    d->flash_state->movies = m;
    dg_flash_instance_t *root = flash_new(m, NULL, NULL, 0, "", 0, false);
    if (!root)
      return;
    root->playing = !s->film_paused;
    dg_avm1_own_set(m->vm, dg_avm1_globals(m->vm), "_root", root->object);
    dg_avm1_own_set(m->vm, dg_avm1_globals(m->vm), "_level0", root->object);
    flash_enter(root, 0, true);
  }
}
bool dg_flash_advance(dg_runtime_t *d, unsigned sprite, unsigned target) {
  dg_flash_instance_t *root = dg_flash_root(d, sprite);
  if (!root)
    return false;
  dg_flash_movie_t *m = root->movie;
  unsigned revision = d->visual_revision;
  if (!flash_run(m, 100000))
    return false;
  if (m->busy || m->queue_count)
    return false;
  unsigned serial = m->director->flash_state->serial;
  // Each pre-existing child advances independently. Newly placed children
  // show their first frame for this opportunity instead of skipping it.
  for (dg_flash_instance_t *i = m->instances; i; i = i->next)
    if (i->alive && i != root && i->id <= serial) {
      flash_actions(i, 0, 2);
      dg_avm1_library_event(m->library, i->object, "onEnterFrame", 0, NULL);
      if (i->playing && i->clip && i->clip->frame_count > 1)
        flash_enter(i, i->frame + 1, true);
    }
  flash_actions(root, 0, 2);
  dg_avm1_library_event(m->library, root->object, "onEnterFrame", 0, NULL);
  unsigned steps = target >= root->frame ? target - root->frame : 1;
  if (steps > 4096) {
    lv_fail(d->values, "Flash stream frame catch-up exceeded budget");
    return false;
  }
  for (unsigned n = 0; n < steps && root->playing && !d->values->failed; n++) {
    bool looping = m->member->film_loop;
    for (unsigned k = 0; k < d->loop_count; k++)
      if (d->loop_members[k] == m->member->id)
        looping = d->loop_values[k];
    if (!looping && root->frame + 1 >= root->clip->frame_count) {
      dg_flash_play(d, sprite, false);
      break;
    }
    flash_enter(root, root->frame + 1, true);
    if (!flash_run(m, 100000))
      break;
    if (m->busy || m->queue_count)
      break;
  }
  flash_run(m, 100000);
  return revision != d->visual_revision;
}
bool dg_flash_service(dg_runtime_t *d, unsigned budget) {
  dg_flash_sync(d);
  if (d->flash_state)
    for (dg_flash_movie_t *m = d->flash_state->movies; m && !d->values->failed;
         m = m->next) {
      if (!flash_run(m, budget))
        return false;
      if (m->pending_enter && !m->busy && !m->queue_count) {
        unsigned frame = m->pending_frame;
        m->pending_enter = false;
        flash_enter(m->root, frame, true);
      }
      if (m->last_tick != d->ticks) {
        m->last_tick = d->ticks;
        dg_avm1_library_service(m->library, (uint64_t)d->ticks * 1000 / 60);
      }
    }
  return !d->values->failed;
}

static bool flash_layer_live(dg_flash_instance_t *parent,
                             const dg_film_layer_t *p) {
  if (!p->clip_id && !p->button_id && !p->field_id && (!p->name || !*p->name))
    return true;
  dg_flash_instance_t *current = dg_flash_child(parent, p->channel);
  if (current)
    return true;
  return false;
}
unsigned dg_flash_layer_count(dg_flash_instance_t *parent) {
  if (!parent || !parent->alive)
    return 0;
  const dg_film_frame_t *frame = flash_frame(parent);
  unsigned count = 0;
  if (frame)
    for (unsigned n = 0; n < frame->count; n++)
      count += flash_layer_live(parent, &frame->layers[n]);
  for (dg_flash_instance_t *i = parent->movie->instances; i; i = i->next)
    if (i->alive && i->dynamic && i->parent == parent) {
      bool replaced = false;
      if (frame)
        for (unsigned n = 0; n < frame->count; n++)
          replaced |= frame->layers[n].channel == i->depth;
      if (!replaced)
        count++;
    }
  return count;
}
bool dg_flash_layer_at(dg_flash_instance_t *parent, unsigned index,
                       dg_flash_layer_view_t *out) {
  if (!parent || !parent->alive)
    return false;
  const dg_film_frame_t *frame = flash_frame(parent);
  unsigned previous = 0;
  bool first = true;
  for (unsigned at = 0; at <= index; at++) {
    unsigned best = UINT32_MAX;
    const dg_film_layer_t *layer = NULL;
    dg_flash_instance_t *instance = NULL;
    if (frame)
      for (unsigned n = 0; n < frame->count; n++) {
        const dg_film_layer_t *p = &frame->layers[n];
        if (!flash_layer_live(parent, p))
          continue;
        if ((first || p->channel > previous) && p->channel < best) {
          best = p->channel;
          layer = p;
          instance = dg_flash_child(parent, best);
        }
      }
    for (dg_flash_instance_t *i = parent->movie->instances; i; i = i->next)
      if (i->alive && i->dynamic && i->parent == parent &&
          (first || i->depth > previous) && i->depth <= best) {
        best = i->depth;
        instance = i;
        layer = NULL;
      }
    if (best == UINT32_MAX)
      return false;
    if (at == index) {
      *out = (dg_flash_layer_view_t){layer, instance, best};
      return true;
    }
    first = false;
    previous = best;
  }
  return false;
}

static bool flash_definition_hit(dg_flash_movie_t *m,
                                 const dg_film_frame_t *frame, float x, float y,
                                 unsigned nesting, bool geometric) {
  if (!frame || nesting >= 32)
    return false;
  for (unsigned n = frame->count; n--;) {
    const dg_film_layer_t *p = &frame->layers[n];
    float px = x, py = y, matrix[6] = {1, 0, 0, 1, 0, 0};
    if (p->affine)
      for (unsigned k = 0; k < 6; k++)
        matrix[k] = p->matrix[k] / 65536.0f;
    if (!flash_inverse(matrix, &px, &py))
      continue;
    bool hit = false;
    if (p->asset && *p->asset && p->value.width && p->value.height) {
      float u = (px - p->value.x) / p->value.width,
            v = (py - p->value.y) / p->value.height;
      if (u >= 0 && v >= 0 && u < 1 && v < 1) {
        dg_member_t member = {.type = 1,
                              .asset = geometric ? p->mask_asset : p->asset,
                              .width = p->value.width,
                              .height = p->value.height};
        if (geometric && (!member.asset || !*member.asset)) {
          lv_fail(m->director->values,
                  "Flash geometric mask hit asset is missing");
          return false;
        }
        hit = !m->director->platform.hit ||
              m->director->platform.hit(m->director->context, &member, 8,
                                        (int)(u * member.width),
                                        (int)(v * member.height));
      }
    } else if (p->clip_id) {
      const dg_flash_clip_t *clip = flash_clip(m->member, p->clip_id);
      if (clip && clip->frames)
        hit = flash_definition_hit(m, &clip->frames[0], px, py, nesting + 1,
                                   geometric);
    } else if (p->button_id) {
      const dg_flash_button_t *button = flash_button(m->member, p->button_id);
      if (button)
        hit = flash_definition_hit(m, &button->states[3], px, py, nesting + 1,
                                   geometric);
    }
    if (hit) {
      for (unsigned mask = 0; mask < n; mask++)
        if (frame->layers[mask].clip_depth >= p->channel &&
            frame->layers[mask].clip_depth) {
          dg_film_layer_t coverage = frame->layers[mask];
          coverage.clip_depth = 0;
          dg_film_frame_t single = {1, &coverage};
          if (!flash_definition_hit(m, &single, x, y, nesting + 1, true)) {
            hit = false;
            break;
          }
        }
      if (hit && !p->clip_depth)
        return true;
    }
  }
  return false;
}
static bool flash_graphics_hit(dg_flash_instance_t *i, float x, float y,
                               bool geometric) {
  for (unsigned p = 0; p < i->path_count; p++) {
    const dg_flash_path_t *path = &i->paths[p];
    const dg_flash_point_t *points = i->points + path->first;
    if (path->filled && path->count >= 3) {
      bool inside = false;
      for (unsigned n = 0, previous = path->count - 1; n < path->count;
           previous = n++) {
        float ax = points[n].x, ay = points[n].y, bx = points[previous].x,
              by = points[previous].y;
        if ((ay > y) != (by > y) && x < (bx - ax) * (y - ay) / (by - ay) + ax)
          inside = !inside;
      }
      if (inside)
        return true;
    }
    if (path->stroked && !geometric)
      for (unsigned n = 1; n < path->count; n++) {
        float ax = points[n - 1].x, ay = points[n - 1].y, dx = points[n].x - ax,
              dy = points[n].y - ay;
        float length = dx * dx + dy * dy,
              t = length ? ((x - ax) * dx + (y - ay) * dy) / length : 0;
        t = fmaxf(0, fminf(1, t));
        float xx = x - ax - t * dx, yy = y - ay - t * dy,
              radius = fmaxf(1, path->line_width) / 2;
        if (xx * xx + yy * yy <= radius * radius)
          return true;
      }
  }
  return false;
}
static bool flash_layer_hit(dg_flash_instance_t *parent,
                            const dg_flash_layer_view_t *layer, float x,
                            float y, unsigned nesting, bool geometric) {
  if (layer->instance) {
    if (!flash_inverse(layer->instance->matrix, &x, &y))
      return false;
    return flash_display_hit(layer->instance, x, y, nesting + 1, geometric);
  }
  if (!layer->layer)
    return false;
  dg_film_layer_t p = *layer->layer;
  p.clip_depth = 0;
  dg_film_frame_t frame = {1, &p};
  return flash_definition_hit(parent->movie, &frame, x, y, nesting + 1,
                              geometric);
}
static bool flash_masks_hit(dg_flash_instance_t *parent, unsigned depth,
                            float x, float y, unsigned nesting) {
  unsigned count = dg_flash_layer_count(parent);
  for (unsigned n = 0; n < count; n++) {
    dg_flash_layer_view_t mask;
    if (!dg_flash_layer_at(parent, n, &mask))
      continue;
    if (mask.layer && mask.layer->clip_depth && mask.depth < depth &&
        mask.layer->clip_depth >= depth &&
        !flash_layer_hit(parent, &mask, x, y, nesting + 1, true))
      return false;
  }
  return true;
}
static bool flash_display_hit(dg_flash_instance_t *i, float x, float y,
                              unsigned nesting, bool geometric) {
  if (!i || !i->alive || nesting >= 32)
    return false;
  if (i->field && geometric &&
      !dg_avm1_truth(i->movie->vm,
                     dg_avm1_get(i->movie->vm, i->object, "embedFonts")))
    return false;
  if (i->field && geometric) {
    if (x < i->bounds[0] || y < i->bounds[1] ||
        x >= i->bounds[0] + i->bounds[2] || y >= i->bounds[1] + i->bounds[3])
      return false;
    dg_runtime_t *d = i->movie->director;
    if (!d->platform.flash_field_hit) {
      lv_fail(d->values, "Flash embedded-text mask hit provider unavailable");
      return false;
    }
    return d->platform.flash_field_hit(d->context, d, i, x, y);
  }
  if (i->field)
    return x >= i->bounds[0] && y >= i->bounds[1] &&
           x < i->bounds[0] + i->bounds[2] && y < i->bounds[1] + i->bounds[3];
  if (i->placement && i->placement->asset) {
    dg_film_layer_t p = *i->placement;
    p.affine = false;
    p.clip_depth = 0;
    dg_film_frame_t frame = {1, &p};
    if (flash_definition_hit(i->movie, &frame, x, y, nesting + 1, geometric))
      return true;
  }
  if (flash_graphics_hit(i, x, y, geometric))
    return true;
  unsigned count = dg_flash_layer_count(i);
  for (unsigned n = count; n--;) {
    dg_flash_layer_view_t layer;
    if (!dg_flash_layer_at(i, n, &layer) ||
        (layer.layer && layer.layer->clip_depth))
      continue;
    if (flash_layer_hit(i, &layer, x, y, nesting + 1, geometric) &&
        flash_masks_hit(i, layer.depth, x, y, nesting + 1))
      return true;
  }
  return false;
}
static dg_flash_instance_t *flash_pointer_target(dg_flash_instance_t *parent,
                                                 float x, float y,
                                                 unsigned nesting) {
  if (!parent || !parent->alive || !parent->visible || nesting >= 32)
    return NULL;
  unsigned count = dg_flash_layer_count(parent);
  for (unsigned n = count; n--;) {
    dg_flash_layer_view_t layer;
    if (!dg_flash_layer_at(parent, n, &layer) || !layer.instance)
      continue;
    dg_flash_instance_t *child = layer.instance;
    if (!child->alive || !child->visible ||
        (layer.layer && layer.layer->clip_depth))
      continue;
    if (!flash_masks_hit(parent, layer.depth, x, y, nesting + 1))
      continue;
    float xx = x, yy = y;
    if (!flash_inverse(child->matrix, &xx, &yy))
      continue;
    if (child->button) {
      if (flash_definition_hit(child->movie, &child->button->states[3], xx, yy,
                               0, false))
        return child;
    } else {
      dg_flash_instance_t *hit =
          flash_pointer_target(child, xx, yy, nesting + 1);
      if (hit)
        return hit;
      lv_t press = dg_avm1_get(child->movie->vm, child->object, "onPress");
      lv_t enabled = dg_avm1_get(child->movie->vm, child->object, "enabled");
#ifdef DIRECTOR64_FLASH_GEOMETRY_TRACE
      if (dg_avm1_callable(child->movie->vm, press))
        fprintf(stderr, "FLASH_HIT %s %.2f,%.2f enabled%d hit%d\n", child->name,
                xx, yy, !enabled || dg_avm1_truth(child->movie->vm, enabled),
                flash_display_hit(child, xx, yy, 0, false));
#endif
      if ((!enabled || dg_avm1_truth(child->movie->vm, enabled)) &&
          dg_avm1_callable(child->movie->vm, press) &&
          flash_display_hit(child, xx, yy, 0, false))
        return child;
    }
  }
  return NULL;
}
static void flash_button_event(dg_flash_instance_t *i, unsigned condition,
                               const char *callback, unsigned state) {
  if (!i || !i->alive)
    return;
  if (i->button) {
    if (i->frame != state) {
      i->frame = state;
      flash_sync_children(i);
      flash_changed(i);
    }
    for (unsigned n = 0; n < i->button->action_count; n++)
      if (i->button->actions[n].event & condition)
        flash_queue(i->parent ? i->parent : i, &i->button->actions[n]);
  }
  uint32_t clip_event = !strcmp(callback, "onPress")            ? 0x400
                        : !strcmp(callback, "onRelease")        ? 0x800
                        : !strcmp(callback, "onReleaseOutside") ? 0x1000
                        : !strcmp(callback, "onRollOver")       ? 0x2000
                        : !strcmp(callback, "onRollOut")        ? 0x4000
                        : !strcmp(callback, "onDragOver")       ? 0x8000
                                                                : 0x10000;
  flash_actions(i, 0, clip_event);
  dg_avm1_library_event(i->movie->library, i->object, callback, 0, NULL);
}
void dg_flash_pointer(dg_runtime_t *d, int stage_x, int stage_y, bool down) {
  if (!d->flash_state)
    return;
  unsigned channel = dg_hit(d, stage_x, stage_y);
  for (dg_flash_movie_t *m = d->flash_state->movies; m && !d->values->failed;
       m = m->next) {
    const dg_sprite_t *sprite = &d->sprites[m->sprite];
    float quad[8], u = 0, v = 0;
    dg_sprite_quad(d, sprite, quad);
    bool inside = dg_quad_uv(quad, stage_x + 0.5f, stage_y + 0.5f, &u, &v);
    // Extend the affine map outside the root for drag/release capture.
    if (!inside) {
      float matrix[6] = {quad[2] - quad[0], quad[3] - quad[1],
                         quad[6] - quad[0], quad[7] - quad[1],
                         quad[0],           quad[1]};
      u = stage_x + 0.5f;
      v = stage_y + 0.5f;
      flash_inverse(matrix, &u, &v);
    }
    if (sprite->flip_h)
      u = 1 - u;
    if (sprite->flip_v)
      v = 1 - v;
    float x = u * m->member->width, y = v * m->member->height;
    bool move = x != m->mouse_x || y != m->mouse_y,
         press = down && !m->mouse_down, release = !down && m->mouse_down;
    m->mouse_x = x;
    m->mouse_y = y;
    m->mouse_down = down;
    dg_avm1_library_pointer(m->library, (int)x, (int)y, down ? 1 : 0);
    for (dg_flash_instance_t *i = m->instances; i; i = i->next)
      if (i->alive) {
        if (move) {
          flash_actions(i, 0, 8);
          dg_avm1_library_event(m->library, i->object, "onMouseMove", 0, NULL);
        }
        if (press) {
          flash_actions(i, 0, 16);
          dg_avm1_library_event(m->library, i->object, "onMouseDown", 0, NULL);
        }
        if (release) {
          flash_actions(i, 0, 32);
          dg_avm1_library_event(m->library, i->object, "onMouseUp", 0, NULL);
        }
      }
    if (m->drag && m->drag->alive) {
      float dx = x, dy = y;
      if (m->drag->parent)
        flash_point_local(m->drag->parent, &dx, &dy);
      dx += m->drag_dx;
      dy += m->drag_dy;
      if (m->drag_bounded) {
        dx = fmaxf(m->drag_bounds[0], fminf(m->drag_bounds[2], dx));
        dy = fmaxf(m->drag_bounds[1], fminf(m->drag_bounds[3], dy));
      }
      m->drag->matrix[4] = dx;
      m->drag->matrix[5] = dy;
      m->drag->overridden |= 3;
      flash_changed(m->drag);
    }
    dg_flash_instance_t *hover = inside && channel == m->sprite
                                     ? flash_pointer_target(m->root, x, y, 0)
                                     : NULL;
    if (hover != m->hover) {
      if (m->hover)
        flash_button_event(m->hover, down ? 16 : 2,
                           down ? "onDragOut" : "onRollOut", 0);
      if (hover)
        flash_button_event(hover, down ? 32 : 1,
                           down ? "onDragOver" : "onRollOver", down ? 2 : 1);
      m->hover = hover;
    }
    if (press && hover) {
      m->pressed = hover;
      flash_button_event(hover, 4, "onPress", 2);
    }
    if (release && m->pressed) {
      bool over = hover == m->pressed;
      flash_button_event(m->pressed, over ? 8 : 64,
                         over ? "onRelease" : "onReleaseOutside", over ? 1 : 0);
      m->pressed = NULL;
    }
  }
}
void dg_flash_key(dg_runtime_t *d, unsigned code, unsigned character,
                  bool down) {
  if (!d->flash_state)
    return;
  unsigned key = character;
  switch (code) {
  case 123:
    key = 37;
    break;
  case 124:
    key = 39;
    break;
  case 125:
    key = 40;
    break;
  case 126:
    key = 38;
    break;
  case 36:
    key = 13;
    break;
  case 48:
    key = 9;
    break;
  case 49:
    key = 32;
    break;
  case 51:
    key = 8;
    break;
  case 53:
    key = 27;
    break;
  case 56:
  case 60:
    key = 16;
    break;
  case 59:
  case 62:
    key = 17;
    break;
  case 58:
  case 61:
    key = 18;
    break;
  default:
    break;
  }
  if (key >= 'a' && key <= 'z')
    key -= 32;
  for (dg_flash_movie_t *m = d->flash_state->movies; m && !d->values->failed;
       m = m->next) {
    dg_avm1_library_key(m->library, key, character, down);
    for (dg_flash_instance_t *i = m->instances; i; i = i->next)
      if (i->alive) {
        flash_actions(i, 0, down ? 64 : 128);
        dg_avm1_library_event(m->library, i->object,
                              down ? "onKeyDown" : "onKeyUp", 0, NULL);
        if (down && i->placement)
          for (unsigned n = 0; n < i->placement->action_count; n++) {
            const dg_flash_action_t *a = &i->placement->actions[n];
            if ((a->event & 0x20000) && a->key == character)
              flash_queue(i, a);
          }
        if (down && i->button)
          for (unsigned n = 0; n < i->button->action_count; n++) {
            const dg_flash_action_t *a = &i->button->actions[n];
            if (((a->event >> 9) & 127) == character && character)
              flash_queue(i->parent ? i->parent : i, a);
          }
      }
  }
}

static bool flash_voice_active(dg_flash_movie_t *m, flash_voice_t *voice) {
  dg_flash_audio_t *v = &voice->value;
  if (!v->playing || !v->sound)
    return false;
  v->position = (uint64_t)(uint32_t)(m->director->ticks - voice->tick) *
                v->sound->rate / 60;
  uint64_t duration = (uint64_t)(v->end_sample - v->start_sample) * v->loops;
  if (v->position >= duration) {
    v->playing = false;
    return false;
  }
  return true;
}
static void flash_sound_events(dg_flash_instance_t *i, unsigned frame) {
  if (!i->clip)
    return;
  dg_flash_movie_t *m = i->movie;
  for (unsigned n = 0; n < i->clip->sound_event_count; n++) {
    const dg_flash_sound_event_t *event = &i->clip->sound_events[n];
    if (event->frame != frame)
      continue;
    const dg_flash_sound_t *sound = NULL;
    for (unsigned k = 0; k < m->member->flash_sound_count; k++)
      if (m->member->flash_sounds[k].id == event->sound)
        sound = &m->member->flash_sounds[k];
    if (!sound || !sound->asset || !*sound->asset || !sound->rate) {
      lv_fail(m->director->values, "Flash event sound definition missing");
      return;
    }
    bool exists = false;
    unsigned free_slot = FLASH_VOICE_LIMIT;
    for (unsigned k = 0; k < FLASH_VOICE_LIMIT; k++) {
      bool active = flash_voice_active(m, &m->voices[k]);
      if (active && m->voices[k].value.sound == sound) {
        exists = true;
        if (event->flags & 32) {
          m->voices[k].value.playing = false;
          active = false;
        }
      }
      if (!active && free_slot == FLASH_VOICE_LIMIT)
        free_slot = k;
    }
    if ((event->flags & 32) || ((event->flags & 16) && exists))
      continue;
    unsigned start = event->flags & 1 ? event->in_point : 0;
    uint64_t end =
        event->flags & 2 ? (uint64_t)event->out_point + 1 : sound->samples;
    if (end > sound->samples)
      end = sound->samples;
    unsigned loops = event->flags & 4 ? event->loops : 1;
    if (start >= end || !loops)
      continue;
    if (free_slot == FLASH_VOICE_LIMIT || flash_voice_serial == UINT32_MAX) {
      lv_fail(m->director->values, "Flash event audio voice capacity");
      return;
    }
    m->voices[free_slot] =
        (flash_voice_t){.tick = m->director->ticks,
                        .value = {.id = ++flash_voice_serial,
                                  .sprite = m->sprite,
                                  .sound = sound,
                                  .event = event,
                                  .start_sample = start,
                                  .end_sample = (uint32_t)end,
                                  .loops = (uint16_t)loops,
                                  .gain = 1,
                                  .playing = true}};
  }
}
unsigned dg_flash_audio_count(dg_runtime_t *d) {
  unsigned count = 0;
  if (d->flash_state)
    for (dg_flash_movie_t *m = d->flash_state->movies; m; m = m->next)
      for (unsigned n = 0; n < FLASH_VOICE_LIMIT; n++)
        count += flash_voice_active(m, &m->voices[n]);
  return count;
}
bool dg_flash_audio_at(dg_runtime_t *d, unsigned index, dg_flash_audio_t *out) {
  if (d->flash_state)
    for (dg_flash_movie_t *m = d->flash_state->movies; m; m = m->next)
      for (unsigned n = 0; n < FLASH_VOICE_LIMIT; n++)
        if (flash_voice_active(m, &m->voices[n])) {
          if (!index--) {
            *out = m->voices[n].value;
            return true;
          }
        }
  return false;
}

unsigned dg_flash_graphics(dg_flash_instance_t *i,
                           const dg_flash_path_t **paths,
                           const dg_flash_point_t **points) {
  if (!i || !i->alive) {
    *paths = NULL;
    *points = NULL;
    return 0;
  }
  *paths = i->paths;
  *points = i->points;
  return i->path_count;
}
static uint32_t flash_draw_color(dg_avm1_t *vm, lv_t color, lv_t alpha) {
  uint32_t rgb = (uint32_t)dg_avm1_number(vm, color) & 0xffffffu;
  double a = lv_type(alpha) == LV_VOID ? 100 : dg_avm1_number(vm, alpha);
  if (!isfinite(a))
    a = 0;
  unsigned coverage = (unsigned)(fmax(0, fmin(100, a)) * 255 / 100);
  return ((rgb >> 16) & 255) | (rgb & 0xff00) | ((rgb & 255) << 16) |
         (coverage << 24);
}
static bool flash_draw_point(dg_flash_instance_t *i, float x, float y) {
  if (!isfinite(x) || !isfinite(y) || fabsf(x) > 1000000 ||
      fabsf(y) > 1000000) {
    lv_fail(i->movie->director->values, "invalid Flash drawing coordinate");
    return false;
  }
  if (i->point_count == i->point_capacity) {
    unsigned capacity = i->point_capacity ? i->point_capacity * 2 : 16;
    if (capacity > 8192) {
      lv_fail(i->movie->director->values, "Flash drawing point capacity");
      return false;
    }
    dg_flash_point_t *points = realloc(i->points, capacity * sizeof(*points));
    if (!points) {
      lv_fail(i->movie->director->values, "Flash drawing point allocation");
      return false;
    }
    i->points = points;
    i->point_capacity = capacity;
  }
  i->points[i->point_count++] = (dg_flash_point_t){x, y};
  i->paths[i->path_count - 1].count++;
  i->pen_x = x;
  i->pen_y = y;
  flash_changed(i);
  return true;
}
static bool flash_draw_path(dg_flash_instance_t *i, float x, float y) {
  if (i->path_count == i->path_capacity) {
    unsigned capacity = i->path_capacity ? i->path_capacity * 2 : 8;
    if (capacity > 1024) {
      lv_fail(i->movie->director->values, "Flash drawing path capacity");
      return false;
    }
    dg_flash_path_t *paths = realloc(i->paths, capacity * sizeof(*paths));
    if (!paths) {
      lv_fail(i->movie->director->values, "Flash drawing path allocation");
      return false;
    }
    i->paths = paths;
    i->path_capacity = capacity;
  }
  i->paths[i->path_count] = i->pen;
  i->paths[i->path_count].first = i->point_count;
  i->paths[i->path_count].count = 0;
  i->path_count++;
  i->path_open = true;
  return flash_draw_point(i, x, y);
}
static bool flash_drawing(dg_flash_instance_t *i, const char *method,
                          unsigned argc, const lv_t *argv, lv_t *out) {
  dg_avm1_t *vm = i->movie->vm;
  *out = 0;
  if (!strcmp(method, "lineStyle")) {
    i->pen.stroked = argc > 0;
    i->path_open = false;
    if (argc) {
      double width = dg_avm1_number(vm, argv[0]);
      if (!isfinite(width) || width < 0 || width > 255) {
        lv_fail(i->movie->director->values, "Flash line width out of range");
        return true;
      }
      i->pen.line_width = (float)width;
      i->pen.line = flash_draw_color(vm, argc > 1 ? argv[1] : lv_num(0),
                                     argc > 2 ? argv[2] : 0);
    }
    return true;
  }
  if (!strcmp(method, "beginFill")) {
    i->pen.filled = argc > 0;
    i->path_open = false;
    if (argc)
      i->pen.fill = flash_draw_color(vm, argv[0], argc > 1 ? argv[1] : 0);
    return true;
  }
  if (!strcmp(method, "endFill")) {
    i->pen.filled = false;
    i->path_open = false;
    return true;
  }
  if (!strcmp(method, "moveTo")) {
    if (argc >= 2)
      flash_draw_path(i, (float)dg_avm1_number(vm, argv[0]),
                      (float)dg_avm1_number(vm, argv[1]));
    return true;
  }
  if (!strcmp(method, "lineTo")) {
    if (argc >= 2) {
      if (!i->path_open && !flash_draw_path(i, i->pen_x, i->pen_y))
        return true;
      flash_draw_point(i, (float)dg_avm1_number(vm, argv[0]),
                       (float)dg_avm1_number(vm, argv[1]));
    }
    return true;
  }
  if (!strcmp(method, "clear")) {
    i->path_count = i->point_count = 0;
    i->path_open = false;
    i->pen = (dg_flash_path_t){0};
    i->pen_x = i->pen_y = 0;
    flash_changed(i);
    return true;
  }
  return false;
}
