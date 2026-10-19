#ifndef DIRECTOR64_WEB_COMPOSITOR_PREFIX_H
#define DIRECTOR64_WEB_COMPOSITOR_PREFIX_H
#if DG_D10
// Cache only the immutable leading stack. Live providers stop the prefix;
// their clocks, fields and child state remain in the ordinary render path.
static bool prefix_member(wc_film_key_t *key, dg_runtime_t *d,
                          const dg_member_t *member) {
  if (!member || member->source_xtra || member->flash_field_count ||
      member->flash_clip_count || member->flash_direct_to_stage ||
      dg_member_image(d, member->id))
    return false;
  if (member->type != 1 && member->type != 5 && member->type != 8 &&
      member->type != 12)
    return false;
  film_key_member(key, d, member);
  if (member->type == 8)
    film_key_string(key,
                    dg_shape_texture(d, member, dg_member_pattern(d, member)));
  return key->valid;
}

static wc_film_key_t prefix_key(wc_t *c, dg_runtime_t *d, const uint16_t *order,
                                unsigned count, unsigned *end) {
  wc_film_key_t key = {.valid = true};
  *end = 0;
  if (!d->movie || !dg_stage_palette(d) ||
      c->image_budget < sizeof(c->pixels) || d->trail_count) {
    key.valid = false;
    return key;
  }
  film_key_add(&key, &d, sizeof(d));
  film_key_add(&key, &d->movie, sizeof(d->movie));
  key.identity_bytes = key.count;
  film_key_add(&key, &d->stage_color, sizeof(d->stage_color));
  film_key_add(&key, dg_stage_palette(d), 256 * sizeof(uint32_t));
  uint64_t area = 0;
  unsigned providers = 0;
  for (unsigned draw = 0; draw < count; draw++) {
    unsigned channel = order[draw];
    const dg_sprite_t *sprite = &d->sprites[channel];
    const dg_member_t *member = dg_member(d, sprite->value.member);
    // Do not append a partially accepted provider to the preceding key.
    size_t accepted = key.count;
    if (!prefix_member(&key, d, member)) {
      key.count = accepted;
      break;
    }
    if (sprite->value.ink == 9) {
      const dg_member_t *mask = dg_member(d, member->id + 1);
      film_key_add(&key, &mask, sizeof(mask));
      if (mask && mask->type == 1 && !prefix_member(&key, d, mask)) {
        key.count = accepted;
        break;
      }
    }
    film_key_add(&key, &channel, sizeof(channel));
    film_key_add(&key, sprite, sizeof(*sprite));
    int l, t, r, b;
    dg_sprite_bounds(d, sprite, &l, &t, &r, &b);
    if (l < 0)
      l = 0;
    if (t < 0)
      t = 0;
    if (r > WC_WIDTH)
      r = WC_WIDTH;
    if (b > WC_HEIGHT)
      b = WC_HEIGHT;
    if (r > l && b > t) {
      area += (uint64_t)(r - l) * (b - t);
      providers++;
    }
    *end = draw + 1;
  }
  // A full-stage retained surface should replace substantial repeated work.
  if (providers < 2 || area < (uint64_t)WC_WIDTH * WC_HEIGHT / 2)
    key.valid = false;
  return key;
}

static void prefix_capture(wc_t *c, const wc_film_key_t *key,
                           const uint32_t *pixels) {
  uint32_t *copy = malloc(sizeof(c->pixels));
  if (!copy)
    return; // Optional acceleration; the completed picture is valid.
  memcpy(copy, pixels, sizeof(c->pixels));
  wc_image_t image = {.width = WC_WIDTH,
                      .height = WC_HEIGHT,
                      .cover_r = WC_WIDTH,
                      .cover_b = WC_HEIGHT,
                      .pixels = copy,
                      .follow_alpha = true};
  if (plane_keep(c, 3, key->data, key->count, key->identity_bytes, &image))
    c->prefix_builds++;
  else
    free(copy);
}
#endif
#endif
