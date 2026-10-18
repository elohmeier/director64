// Exact pixels of an expensive immutable prefix of the top-level stage.
// This is optional storage: dynamic providers and allocation pressure retain
// the ordinary renderer, including all of its compositing/rounding rules.
#if DG_D10
typedef struct {
  uint16_t channel;
  dg_sprite_t sprite;
  const dg_member_t *member, *mask;
  uint32_t member_id, mask_id;
  dg_member_t member_value, mask_value;
} scene_prefix_entry_t;
static surface_t scene_prefix_plane;
static scene_prefix_entry_t *scene_prefix_entries;
static unsigned scene_prefix_count, scene_prefix_first, scene_prefix_end;
static const dg_runtime_t *scene_prefix_owner;
static const dg_movie_t *scene_prefix_movie;
static uint32_t scene_prefix_background, scene_prefix_palette[256];
static bool scene_prefix_valid;
static dg_runtime_t *scene_prefix_context;
static unsigned scene_prefix_hits, scene_prefix_misses;
static uint64_t scene_prefix_copy_us;

static bool scene_prefix_release(void) {
  bool released = scene_prefix_plane.buffer != NULL;
  // Pixels are copied by the CPU; no RDP command refers to this plane.
  if (released)
    surface_free(&scene_prefix_plane);
  free(scene_prefix_entries);
  scene_prefix_entries = NULL;
  memset(&scene_prefix_plane, 0, sizeof(scene_prefix_plane));
  scene_prefix_valid = false;
  scene_prefix_owner = NULL;
  scene_prefix_count = scene_prefix_first = scene_prefix_end = 0;
  return released;
}
static bool scene_prefix_reclaim(void) {
  return scene_prefix_plane.buffer ? scene_prefix_release() : false;
}
static bool scene_prefix_member(dg_runtime_t *d, const dg_sprite_t *sprite,
                                const dg_member_t **member,
                                const dg_member_t **mask) {
  *member = dg_member(d, sprite->value.member);
  *mask = NULL;
  if (!sprite->visible || !sprite->value.type)
    return true;
  if (sprite->trails || sprite->value.trails)
    return false;
  const dg_member_t *m = *member;
  if (!m)
    return !sprite->value.member && sprite->value.type < 16;
  if (m->source_xtra || m->flash_direct_to_stage || m->flash_field_count ||
      dg_member_image(d, m->id))
    return false;
  if (m->type != 1 && m->type != 5 && m->type != 12 && m->type != 8)
    return false;
  // Pattern assets can be replaced independently through a cast's tile
  // catalog. Plain vector shapes have only the snapshot's score properties.
  if (m->type == 8 && dg_member_pattern(d, m) != 1)
    return false;
  if (sprite->value.ink == 9) {
    *mask = dg_member(d, m->id + 1);
    if (*mask && dg_member_image(d, (*mask)->id))
      return false;
  }
  return true;
}
// Return the cached prefix's end, or zero when ordinary rendering is cheaper
// or required. `hit` distinguishes restoration from the first capture.
static unsigned scene_prefix_begin(surface_t *fb, dg_runtime_t *d,
                                   const uint16_t *order, unsigned first,
                                   unsigned count, bool *hit) {
  *hit = false;
  if (d != scene_prefix_context || fb->width != 640 || fb->height != 480 ||
      surface_get_format(fb) != FMT_RGBA16 || d->trail_count ||
      transition_active || d->transition_serial != transition_consumed)
    return 0;
  unsigned end = first, expensive_pixels = 0;
  for (; end < count; end++) {
    const dg_sprite_t *sprite = &d->sprites[order[end]];
    const dg_member_t *member, *mask;
    if (!scene_prefix_member(d, sprite, &member, &mask))
      break;
    if (sprite->visible && sprite->value.type && dg_opacity(sprite) &&
        (sprite->value.ink == 9 || dg_ink_arithmetic(sprite->value.ink))) {
      int l, t, r, b;
      dg_sprite_bounds(d, sprite, &l, &t, &r, &b);
      l = dg_prescale_coord(l);
      t = dg_prescale_coord(t);
      r = dg_prescale_coord(r);
      b = dg_prescale_coord(b);
      if (l < 0)
        l = 0;
      if (t < 0)
        t = 0;
      if (r > 640)
        r = 640;
      if (b > 480)
        b = 480;
      if (r > l && b > t)
        expensive_pixels += (unsigned)(r - l) * (unsigned)(b - t);
    }
  }
  if (end == first || expensive_pixels < 32768) {
    scene_prefix_release();
    return 0;
  }
  static const uint32_t empty_palette[256] = {0};
  const uint32_t *palette = dg_stage_palette(d);
  if (!palette)
    palette = empty_palette;
  bool same =
      scene_prefix_valid && scene_prefix_owner == d &&
      scene_prefix_movie == d->movie &&
      scene_prefix_background == d->stage_color &&
      scene_prefix_first == first && scene_prefix_end == end &&
      !memcmp(scene_prefix_palette, palette, sizeof(scene_prefix_palette));
  for (unsigned i = first; same && i < end; i++) {
    const scene_prefix_entry_t *entry = &scene_prefix_entries[i - first];
    const dg_sprite_t *sprite = &d->sprites[order[i]];
    const dg_member_t *member, *mask;
    same =
        scene_prefix_member(d, sprite, &member, &mask) &&
        entry->channel == order[i] && entry->member == member &&
        entry->mask == mask && entry->member_id == (member ? member->id : 0) &&
        entry->mask_id == (mask ? mask->id : 0) &&
        (!member || !memcmp(&entry->member_value, member, sizeof(*member))) &&
        (!mask || !memcmp(&entry->mask_value, mask, sizeof(*mask))) &&
        !memcmp(&entry->sprite, sprite, sizeof(*sprite));
  }
  if (!same) {
    scene_prefix_valid = false;
    if (scene_prefix_count != end - first) {
      free(scene_prefix_entries);
      scene_prefix_entries =
          malloc((end - first) * sizeof(*scene_prefix_entries));
      if (!scene_prefix_entries) {
        scene_prefix_release();
        return 0;
      }
      scene_prefix_count = end - first;
    }
    if (!scene_prefix_plane.buffer) {
      scene_prefix_plane = surface_alloc(FMT_RGBA16, 640, 480);
      while (!scene_prefix_plane.buffer && evict_arena())
        scene_prefix_plane = surface_alloc(FMT_RGBA16, 640, 480);
      if (!scene_prefix_plane.buffer) {
        scene_prefix_release();
        return 0;
      }
    }
    scene_prefix_owner = d;
    scene_prefix_movie = d->movie;
    scene_prefix_background = d->stage_color;
    scene_prefix_first = first;
    scene_prefix_end = end;
    memcpy(scene_prefix_palette, palette, sizeof(scene_prefix_palette));
    for (unsigned i = first; i < end; i++) {
      scene_prefix_entry_t *entry = &scene_prefix_entries[i - first];
      entry->channel = order[i];
      entry->sprite = d->sprites[order[i]];
      scene_prefix_member(d, &entry->sprite, &entry->member, &entry->mask);
      entry->member_id = entry->member ? entry->member->id : 0;
      entry->mask_id = entry->mask ? entry->mask->id : 0;
      if (entry->member)
        entry->member_value = *entry->member;
      if (entry->mask)
        entry->mask_value = *entry->mask;
    }
    scene_prefix_misses++;
    return end;
  }
  uint64_t started = get_ticks_us();
  rdpq_detach_wait();
  for (unsigned y = 0; y < 480; y++)
    memcpy((uint8_t *)fb->buffer + y * fb->stride,
           (const uint8_t *)scene_prefix_plane.buffer +
               y * scene_prefix_plane.stride,
           1280);
  rdpq_attach(fb, NULL);
  scene_prefix_copy_us += get_ticks_us() - started;
  scene_prefix_hits++;
  *hit = true;
  return end;
}
static void scene_prefix_capture(surface_t *fb) {
  if (!scene_prefix_plane.buffer || !scene_prefix_owner)
    return;
  uint64_t started = get_ticks_us();
  rdpq_detach_wait();
  for (unsigned y = 0; y < 480; y++)
    memcpy((uint8_t *)scene_prefix_plane.buffer + y * scene_prefix_plane.stride,
           (const uint8_t *)fb->buffer + y * fb->stride, 1280);
  rdpq_attach(fb, NULL);
  scene_prefix_copy_us += get_ticks_us() - started;
  scene_prefix_valid = true;
}
#endif
