#ifndef DIRECTOR64_WEB_COMPOSITOR_FILM_H
#define DIRECTOR64_WEB_COMPOSITOR_FILM_H
#if DG_D10
typedef struct {
  unsigned char *data;
  size_t count, capacity, identity_bytes;
  bool valid;
} wc_film_key_t;

static void film_key_add(wc_film_key_t *key, const void *data, size_t bytes) {
  if (!key->valid)
    return;
  if (bytes > 1024u * 1024u - key->count) {
    key->valid = false;
    return;
  }
  if (key->count + bytes > key->capacity) {
    size_t capacity = key->capacity ? key->capacity * 2 : 2048;
    if (capacity < key->count + bytes)
      capacity = key->count + bytes;
    void *grown = realloc(key->data, capacity);
    if (!grown) {
      key->valid = false;
      return;
    }
    key->data = grown;
    key->capacity = capacity;
  }
  memcpy(key->data + key->count, data, bytes);
  key->count += bytes;
}
static void film_key_string(wc_film_key_t *key, const char *text) {
  if (!text)
    text = "";
  film_key_add(key, text, strlen(text) + 1);
}
static void film_key_member(wc_film_key_t *key, dg_runtime_t *d,
                            const dg_member_t *m) {
  film_key_add(key, m, sizeof(*m));
  film_key_string(key, m->asset);
  unsigned pattern = dg_member_pattern(d, m);
  film_key_add(key, &pattern, sizeof(pattern));
  const dg_pixel_image_t *mutable = dg_member_image(d, m->id);
  film_key_add(key, &mutable, sizeof(mutable));
  if (mutable) {
    film_key_add(key, mutable, sizeof(*mutable));
    film_key_add(key, &d->visual_revision, sizeof(d->visual_revision));
  }
  // Field text/font/color/scroll edits have their own live storage. Keep
  // those films reusable only while the runtime's visual revision agrees.
  if (m->type == 3 || m->type == 7) {
    film_key_string(key, m->text);
    film_key_add(key, &d->visual_revision, sizeof(d->visual_revision));
  }
}
static void film_key_frame(wc_film_key_t *key, dg_runtime_t *d, unsigned parent,
                           const dg_sprite_t *sprite, const dg_member_t *member,
                           const dg_film_frame_t *frame, unsigned depth) {
  if (!key->valid)
    return;
  if (depth >= 8 || member->source_xtra || member->flash_field_count ||
      member->flash_clip_count) {
    key->valid = false;
    return;
  }
  uint32_t instance = 0, revision = 0;
  if (!dg_film_visual_state(d, parent, sprite, member, &instance, &revision)) {
    key->valid = false;
    return;
  }
  film_key_add(key, &instance, sizeof(instance));
  film_key_add(key, &revision, sizeof(revision));
  film_key_member(key, d, member);
  film_key_add(key, &frame->count, sizeof(frame->count));
  for (unsigned i = 0; i < frame->count && key->valid; i++) {
    const dg_film_layer_t *layer = &frame->layers[i];
    if (layer->affine || (layer->name && *layer->name)) {
      key->valid = false;
      break;
    }
    dg_sprite_t child;
    if (!dg_film_layer_sprite(d, parent, layer, &child))
      continue;
    film_key_add(key, &layer->channel, sizeof(layer->channel));
    film_key_add(key, &child.value, sizeof(child.value));
    film_key_string(key, layer->asset);
    const dg_member_t *m = dg_member(d, child.value.member);
    if (!m)
      continue;
    if (m->type == 9 || dg_is_video(m) || m->source_xtra ||
        m->flash_field_count || m->flash_clip_count) {
      key->valid = false;
      break;
    }
    film_key_member(key, d, m);
    if (child.value.ink == 9) {
      const dg_member_t *mask = dg_member(d, m->id + 1);
      film_key_add(key, &mask, sizeof(mask));
      if (mask)
        film_key_member(key, d, mask);
    }
    if (m->type == 2 && (m->film_track_count || m->film_frames)) {
      dg_film_frame_t nested;
      dg_film_layer_t *owned =
          dg_film_frame_resolve(d, DG_SPRITES, &child, m, &nested);
      if (!owned) {
        key->valid = false;
        break;
      }
      film_key_frame(key, d, DG_SPRITES, &child, m, &nested, depth + 1);
      free(owned);
    } else if (m->type == 2)
      film_key_string(key, dg_film_asset(m, &child));
  }
}
static wc_film_key_t film_key(dg_runtime_t *d, unsigned parent,
                              const dg_sprite_t *sprite,
                              const dg_member_t *member,
                              const dg_film_frame_t *frame) {
  wc_film_key_t key = {.valid = true};
  film_key_add(&key, &d, sizeof(d));
  film_key_add(&key, &d->movie, sizeof(d->movie));
  uint32_t instance = 0, revision = 0;
  if (!dg_film_visual_state(d, parent, sprite, member, &instance, &revision))
    key.valid = false;
  film_key_add(&key, &instance, sizeof(instance));
  key.identity_bytes = key.count;
  const uint32_t *palette = dg_stage_palette(d);
  if (palette)
    film_key_add(&key, palette, 256 * sizeof(*palette));
  film_key_frame(&key, d, parent, sprite, member, frame, 0);
  bool masked = sprite->value.ink == 9 && !dg_film_container_effects(member);
  film_key_add(&key, &masked, sizeof(masked));
  if (masked) {
    const dg_member_t *mask = dg_member(d, member->id + 1);
    film_key_add(&key, &mask, sizeof(mask));
    if (mask)
      film_key_member(&key, d, mask);
  }
  return key;
}
#endif
#endif
