// Named MIAW contexts use the same heap/global table as the stage, with a
// separate score, tempo, lifecycle and suspended interpreter stack. Reuse the
// context/root transport used by linked movies without their host-clock or
// script restrictions. The selected corpus authors only window("control").
static dg_runtime_t *window_root(dg_runtime_t *d) {
  while (d->window_parent) d = d->window_parent;
  return d;
}
static dg_runtime_t *window_target(dg_runtime_t *d) {
  while(d->tell_depth && d->tell_levels[d->tell_depth-1]>d->values->depth)d->tell_depth--;
  return d->tell_depth ? d->tell_targets[d->tell_depth - 1] : d;
}
static dg_runtime_t *window_access_target(dg_runtime_t *d, lv_t owner) {
  dg_runtime_t *target = window_target(d), *root = window_root(d);
  if(lv_type(owner)==LV_SOUND && (d->window_parent || root->window_runtime))return root;
  // A character/word range retains the cast member it addresses, including
  // when a shared controller object sends it back to the stage.
  for(unsigned depth=0;depth<8 && lv_type(owner)==LV_PROPLIST;depth++) {
    lv_t next={0};
    for(unsigned i=1;i<=lv_count(d->values,owner);i++) {
      lv_t key=lv_at(d->values,owner,i*2-1);
      if((lv_type(key)==LV_STRING || lv_type(key)==LV_SYMBOL) && eq(lv_cstr(d->values,key),"_textRange"))
        next=lv_at(d->values,owner,i*2);
    }
    if(lv_type(next)==LV_VOID)break;
    owner=next;
  }
  if (lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_FIELD) {
    unsigned file = DG_ID_FILE((uint32_t)lv_id(owner));
    // A cast member has one mutable value even when addressed through a
    // resident window interface. Prefer the stage for shared cast archives;
    // window-only libraries use the controller's persistent member roots.
    if (root->movie && d10_stage_uses(root, dg_loaded(root, file))) return root;
    if (root->window_runtime && dg_loaded(root->window_runtime, file)) return root->window_runtime;
  }
  return target;
}
dg_runtime_t *dg_member_context(dg_runtime_t *d,uint32_t member) {
  return member ? window_access_target(d,lv_make(LV_MEMBER,(int32_t)member)) : window_root(d);
}
static dg_runtime_t *window_native_begin(dg_runtime_t *base, lv_t owner) {
  if (base->window_native) return NULL;
  dg_runtime_t *target = window_access_target(base, owner);
  if (target == base || !target->linked_state || !linked_state_new(base)) return NULL;
  // Only the native service's member roots change. The caller's Lingo frames
  // and continuation remain active, including nested tell stage/end blocks.
  linked_save(base);
  memcpy(base->values->roots, target->linked_state->roots, sizeof(base->values->roots));
  base->window_native = true;
  return target;
}
static void window_native_end(dg_runtime_t *base, dg_runtime_t *target) {
  if (!target) return;
  memcpy(target->linked_state->roots, base->values->roots, sizeof(base->values->roots));
  memcpy(base->values->roots, base->linked_state->roots, sizeof(base->values->roots));
  base->window_native = false;
  dg_runtime_t *root=window_root(target);
  if (target!=root && root->window_visible && target->visual_revision!=root->window_revision) {
    root->window_revision=target->visual_revision;root->visual_revision++;
  }
  if(root->window_forget && target==root->window_runtime && base!=target && base->values->context==base)
    window_dispose(root);
}
static bool window_tell(dg_runtime_t *d, unsigned ln, unsigned argc, const lv_t *a) {
  if (ln == LN_TELL_END) {
    if (d->tell_depth) d->tell_depth--;
    d->tell_window = d->tell_depth && d->tell_targets[d->tell_depth - 1] != window_root(d);
    return true;
  }
  if (ln != LN_TELL_WINDOW && ln != LN_TELL_STAGE) return false;
  if ((ln == LN_TELL_WINDOW && argc != 1) || (ln == LN_TELL_STAGE && argc)) {
    error(d, "window tell", "argument count"); return true;
  }
  dg_runtime_t *root = window_root(d), *target = root;
  if (ln == LN_TELL_WINDOW) {
    if (lv_type(a[0]) != LV_WINDOW && !eq(lv_cstr(d->values, a[0]), "control")) {
      error(d, "window tell", "unsupported window"); return true;
    }
    target = root->window_runtime;
    if (!target) { error(d, "window tell", "movie not loaded"); return true; }
  }
  if (d->tell_depth == sizeof(d->tell_targets) / sizeof(*d->tell_targets)) {
    error(d, "window tell", "nesting capacity"); return true;
  }
  d->tell_levels[d->tell_depth]=d->values->depth;
  d->tell_targets[d->tell_depth++] = target;
  d->tell_window = target != root;
  return true;
}
dg_runtime_t *dg_window_begin(dg_runtime_t *d) {
  if (d->values->context != d || !d->window_runtime || !d->linked_state) return NULL;
  linked_save(d); linked_restore(d->window_runtime);
  return d->window_runtime;
}
void dg_window_end(dg_runtime_t *d) {
  dg_runtime_t *child = d->values->context;
  if (!child || child->window_parent != d) {
    error(d, "window context", "restore mismatch"); return;
  }
  linked_save(child); linked_restore(d);
}
static bool window_load(dg_runtime_t *d, const dg_movie_t *movie) {
  d = window_root(d);
  lv_runtime_t *r = d->values;
  if (!movie) { error(d, "window movie", "missing target"); return false; }
  if (d->window_runtime && d->window_runtime->movie == movie) return true;
  if(d->window_runtime && r->context==d->window_runtime) {
    // A window cannot replace the interpreter currently servicing its setter.
    // Queue the same ordinary movie cleanup/transition as an authored go.
    snprintf(d->window_runtime->next_movie,sizeof(d->window_runtime->next_movie),"%s",movie->code->name);
    d->window_runtime->next_movie_frame=1;return true;
  }
  const dg_movie_t *shared[DG_FILES]; unsigned count = 0;
  if (!linked_libraries(d, movie, shared, &count) || !linked_state_new(d)) return false;
  dg_runtime_t *child = d->window_runtime;
  if (!child) {
    child = calloc(1, sizeof(*child));
    if (!child) { error(d, "window movie", "allocation failed"); return false; }
    child->values = r; child->platform = d->platform; child->context = d->context;
    child->window_parent = d;
    child->ticks = d->ticks; child->clock = d->clock;
    child->sound_level = d->sound_level; child->color_depth = d->color_depth;
    child->float_precision = d->float_precision;
    child->linked_instance = ++d->linked_next_instance;
    memcpy(child->channel_volume, d->channel_volume, sizeof(child->channel_volume));
    if (!linked_state_new(child)) { free(child); return false; }
    d->window_runtime = child;
  }
  // Loading a fileName runs its movie initialization before the next authored
  // statement can call its global interfaces. Save the *active* interpreter,
  // which may itself be the controller making a request through tell stage.
  dg_runtime_t *active = r->context;
  if (!active || !linked_state_new(active)) return false;
  linked_save(active); linked_restore(child);
  r->depth = 0; r->yielded = r->aborted = false;
  child->tell_depth = 0;
  bool ok = dg_enter(child, movie, 1, shared, count);
  // Loading/telling a window initializes its global interfaces synchronously.
  // A single normal 288-instruction tick can stop half way through startMovie.
  // Keep the initialization bounded independently of the gameplay clock.
  for(unsigned round=0;ok && round<4096;round++) {
    ok=dg_service(child,DG_SERVICE_BUDGET);
    if(!child->start_movie_pending && !r->depth)break;
    if(round==4095) {error(d,"window initialization","instruction budget exceeded");ok=false;}
  }
  linked_save(child); linked_restore(active);
  d->window_movie = movie;
  if (!d->window_home) d->window_home = movie;
  child->window_home = d->window_home;
  snprintf(d->window_movie_name, sizeof(d->window_movie_name), "%s", movie->code->name);
  d->window_open = true; d->window_forget = false;
  d->window_revision=child->visual_revision;
  d->visual_revision++;
  // Sharing immutable code does not broadcast the window's lifecycle into
  // the stage; d10_window_only keeps those event lists separate.
  if (active == d) d10_window_share(d);
  if (d->platform.trace) d->platform.trace(d->context, "WINDOW_MOVIE_ATTACHED");
  return ok && !r->failed;
}
static void window_dispose(dg_runtime_t *d) {
  dg_runtime_t *child = d->window_runtime;
  if (!child) return;
  dg_runtime_t *active = d->values->context;
  if (active && active != child && linked_state_new(active)) {
    linked_save(active); linked_restore(child);
    child->values->depth = 0;
    for (unsigned i = 1; i < DG_SPRITES && !child->values->failed; i++) {
      if (child->sprites[i].value.type && sprite_event(child, "endsprite", i, 0))
        lv_run(child->values, DG_SERVICE_BUDGET);
      child->values->depth = 0;
    }
    if (dg_event(child, "stopmovie", 0)) lv_run(child->values, DG_SERVICE_BUDGET);
    child->values->depth = 0;
    dg_stop_sounds(child);
    linked_save(child); linked_restore(active);
  }
  d->window_runtime = NULL;
  dg_linked_dispose(child); dg_films_dispose(child); dg_images_dispose(child);
  free(child);
  d->window_movie = NULL;
  // Immutable controller code may still be referenced by shared objects.
  // Retaining its home descriptor also keeps its movie events out of stage
  // broadcasts after forget; the independent score/roots have been released.
  d->window_movie_name[0] = d->window_movie_queue[0] = 0;
  d->window_open = d->window_visible = d->window_focus = d->window_pointer = false;
  d->window_forget = false; d->tell_depth = 0;
  d->visual_revision++;
}
static void window_pointer(dg_runtime_t *d, int *x, int *y, bool *down) {
  if (!d->window_runtime) return;
  bool inside = d->window_visible && *x >= d->window_rect[0] && *x < d->window_rect[2] &&
      *y >= d->window_rect[1] && *y < d->window_rect[3];
  bool pressed = *down && !d->window_pointer_down;
  if (pressed) { d->window_focus = inside; d->window_pointer = inside; }
  d->window_pointer_x = inside || d->window_pointer ? *x - d->window_rect[0] : -32768;
  d->window_pointer_y = inside || d->window_pointer ? *y - d->window_rect[1] : -32768;
  d->window_pointer_down = *down;
  if (d->window_pointer || (inside && !d->mouse_down)) { *x = *y = -32768; *down = false; }
}
static bool window_key(dg_runtime_t *d, unsigned code, unsigned character, bool down) {
  dg_runtime_t *child = d->window_runtime;
  if (!child) return false;
  bool held = (child->keys_down[code >> 5] & (UINT32_C(1) << (code & 31))) != 0;
  if ((!down && held) || (down && d->window_visible && d->window_focus)) {
    dg_key(child, code, character, down); return true;
  }
  return false;
}
static bool window_update(dg_runtime_t *d, unsigned budget) {
  if (d->window_parent || !d->window_runtime) return !d->values->failed;
  if (d->window_forget) { window_dispose(d); return !d->values->failed; }
  dg_runtime_t *child = dg_window_begin(d);
  if (!child) return !d->values->failed;
  bool changed_movie=false;
  // Hidden windows still own an independent 60Hz clock and authored tempo.
  if (child->ticks < d->ticks) {
    child->ticks = d->ticks - 1; child->clock = d->clock - 1000000;
    dg_tick(child, d->window_pointer_x, d->window_pointer_y,
        d->window_pointer_down && d->window_pointer, budget);
  } else dg_service(child, budget);
  if (dg_transition_ready(child)) {
    char stem[sizeof(child->next_movie)];
    snprintf(stem, sizeof(stem), "%s", basename_(child->next_movie));
    char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
    const dg_movie_t *movie = d->platform.find_movie ? d->platform.find_movie(d->context, stem, 0) : NULL;
    const dg_movie_t *shared[DG_FILES]; unsigned count = 0;
    if (!movie || !linked_libraries(d, movie, shared, &count)) error(d, "window transition", stem);
    else {
      dg_enter(child, movie, child->next_movie_frame, shared, count);
      d->window_movie = movie;
      changed_movie=true;
      snprintf(d->window_movie_name, sizeof(d->window_movie_name), "%s", movie->code->name);
    }
  }
  dg_update_stage(child);
  dg_window_end(d);
  if(changed_movie)d10_window_share(d);
  if (!d->window_pointer_down) d->window_pointer = false;
  if (child->visual_revision != d->window_revision) {
    d->window_revision=child->visual_revision;
    if (d->window_visible) d->visual_revision++;
  }
  return !d->values->failed;
}
