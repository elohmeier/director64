// The current frame's unique decoded planes, including live film children.
// Bounded metadata is shared because collection finishes before rendering.
#if DG_D10
typedef struct {
  const char *asset;
  uint32_t member, bytes;
  uint16_t width, height;
  uint8_t type, ink;
} image_pending_t;
static image_pending_t image_pending[DG_SPRITES * 2];
static unsigned image_pending_count;
static uint64_t image_pending_bytes, image_pending_temporary;
static bool image_pending_dynamic;

static void image_pending_add(const dg_member_t *member, const char *asset,
                              unsigned ink, unsigned opacity) {
  if (!asset || !*asset) return;
  if (ink == 9 || (ink == 0 && dg_blended_copy_matte(opacity))) ink = 8;
  else if (ink == 36 && member->type != 12) ink = 0;
  if (!(ink == 0 || ink == 8 || ink == 32 || ink == 36 || dg_ink_arithmetic(ink))) return;
  for (unsigned i = 0; i < image_pending_count; i++)
    if (image_pending[i].ink == ink && !strcmp(image_pending[i].asset, asset)) return;
  if (image_pending_count == sizeof(image_pending) / sizeof(*image_pending)) {
    lv_fail(values, "N64 frame image dependency count exceeded"); return;
  }
  dg_pack_entry_t entry;
  unsigned bytes;
  if (pack_index_find(dg_image_key(asset, DG_PACK_INK_AUTHORED), &entry)) bytes = entry.allocation;
  else {
    // Synthetic cache-backed probes have no pack directory. A real missing
    // asset still fails in image(), with its member and asset identity.
    unsigned width, height; dg_prescale_dims(member->width, member->height, &width, &height);
    bytes = 32u + width * height * 4u;
  }
  image_pending[image_pending_count++] = (image_pending_t){asset, member->id, bytes,
      member->width, member->height, member->type == 2 ? 1 : member->type, ink};
  image_pending_bytes += (bytes + 31u) & ~31u;
}

static void image_pending_sprite(dg_runtime_t *d, unsigned channel, const dg_sprite_t *sprite,
                                 const dg_film_layer_t *layer, unsigned depth,
                                 int left, int top, int right, int bottom, uint64_t temporary) {
  if (values->failed || !sprite->visible || !sprite->value.type) return;
  int l, t, r, b; dg_sprite_bounds(d, sprite, &l, &t, &r, &b);
  if (r <= left || b <= top || l >= right || t >= bottom || r <= l || b <= t) return;
  const dg_member_t *member = dg_member(d, sprite->value.member);
  dg_member_t direct;
  if (layer && layer->asset && *layer->asset) {
    direct = (dg_member_t){.type=1, .asset=layer->asset,
        .width=layer->value.width, .height=layer->value.height};
    member = &direct;
  }
  if (!member || dg_member_image(d, member->id)) return;
  bool driven = channel < DG_SPRITES && sprite == &d->sprites[channel] && dg_film_driven(d, channel);
  const dg_sprite_t *film_source = sprite;
  dg_sprite_t container;
  if (dg_film_container_effects(member)) {
    dg_film_container_sprite(&container, sprite);
    sprite = &container;
  }
  if (!dg_opacity(sprite)) return;
  if (member->type == 2 && member->flash_clip_count) {
    // AVM1 display lists can grow independently of authored poses. Their
    // asset provider copies each decoded leaf before requesting the next.
    image_pending_dynamic = true; return;
  }
  if (member->type == 2 && (member->film_frames || member->film_track_count || driven)) {
    if (depth >= 8) { lv_fail(values, "film loop image dependency nesting exceeded"); return; }
    unsigned width, height; dg_prescale_plane_dims(member->width, member->height, &width, &height);
    dg_film_frame_t resolved;
    bool resolve = driven || member->film_track_count;
    dg_film_layer_t *owned = resolve ? dg_film_frame_resolve(d, channel, film_source, member, &resolved) : NULL;
    if (resolve && !owned) return;
    const dg_film_frame_t *frame = resolve ? &resolved : &member->film_frames[dg_film_storage_pose(member, sprite)];
    n64_film_crop_t crop = n64_film_crop(d, channel, member, frame, width, height);
    unsigned tw = crop.width < 256 ? crop.width : 256, th = crop.height < 128 ? crop.height : 128;
    temporary += (uint64_t)crop.width * crop.height * 4u;
    if (tw != crop.width || th != crop.height) temporary += (uint64_t)tw * th * 4u;
    if (temporary > image_pending_temporary) image_pending_temporary = temporary;
    for (unsigned i = 0; i < frame->count && !values->failed; i++) {
      dg_sprite_t child;
      if (dg_film_layer_sprite(d, channel, &frame->layers[i], &child))
        image_pending_sprite(d, DG_SPRITES, &child, &frame->layers[i], depth + 1,
            member->film_left, member->film_top,
            member->film_left + member->width, member->film_top + member->height, temporary);
    }
    free(owned); return;
  }
  if (member->type == 9) { image_pending_dynamic = true; return; }
  if (member->type != 1 && member->type != 5 && member->type != 12 && member->type != 8 &&
      !(member->type == 2 && member->film_count)) return;
  image_pending_add(member, member->type == 2 ? dg_film_asset(member, sprite) : member->asset,
                    member->type == 8 ? 0 : sprite->value.ink, dg_opacity(sprite));
  if (sprite->value.ink == 9 && member->type == 1) {
    const dg_member_t *mask = dg_member(d, member->id + 1);
    if (mask && mask->type == 1 && mask_pair(member, mask) && !dg_member_image(d, mask->id))
      image_pending_add(mask, mask->asset, 8, 100);
  }
}

static void image_preload_scene(dg_runtime_t *d, const uint16_t *order,
                                unsigned first, unsigned count, int ox, int oy) {
  if (image_streaming) return;
  uint64_t preload_started = get_ticks_us();
  image_pending_count = 0; image_pending_bytes = image_pending_temporary = 0;
  image_pending_dynamic = false;
  for (unsigned i = first; i < count; i++)
    image_pending_sprite(d, order[i], &d->sprites[order[i]], NULL, 0,
        -ox, -oy, dg_stage_coord(640)-ox, dg_stage_coord(480)-oy, 0);
  if (values->failed) { phase_preload_us += get_ticks_us()-preload_started; return; }
  heap_stats_t stats; sys_get_heap_stats(&stats);
  uint64_t reserve = image_pending_temporary + 320u * 1024u;
  // Preloading is an optimization. If a frame exceeds simultaneous storage,
  // render in order and release textures only after their RDP reads complete.
  if (image_pending_dynamic || image_pending_count > IMAGE_CACHE_SLOTS ||
      image_pending_bytes + reserve > (uint64_t)stats.free + cache_arena_bytes) {
    image_streaming = true;
    image_stream_boundary();
    phase_preload_us += get_ticks_us()-preload_started;
    return;
  }
  uint64_t missing_bytes = 0;
  for (unsigned i = 0; i < image_pending_count; i++)
    if (!image_cached(image_pending[i].asset, image_pending[i].ink))
      missing_bytes += (image_pending[i].bytes + 31u) & ~31u;
  // Resident images already occupy their allocations. Charging them again
  // would force a full eviction on every live-film frame even when the scene
  // fits. Probe only new planes and the actual simultaneous intermediates.
  uint64_t allocation = missing_bytes + image_pending_temporary;
  if (allocation) {
    void *probe = allocation <= UINT32_MAX ? memalign(32, (size_t)allocation) : NULL;
    bool compact = !probe; free(probe);
    if (compact) { rspq_wait(); clear_text_fonts(); while (evict()) {} }
  }
  while (image_pending_count && !values->failed) {
    unsigned largest = 0;
    for (unsigned i = 1; i < image_pending_count; i++)
      if (image_pending[i].bytes > image_pending[largest].bytes) largest = i;
    image_pending_t pending = image_pending[largest];
    image_pending[largest] = image_pending[--image_pending_count];
    dg_member_t member = {.id=pending.member, .asset=pending.asset, .width=pending.width,
        .height=pending.height, .type=pending.type};
    image(&member, pending.ink);
  }
  phase_preload_us += get_ticks_us()-preload_started;
}
#endif
