#ifndef DIRECTOR64_WEB_COMPOSITOR_CACHE_H
#define DIRECTOR64_WEB_COMPOSITOR_CACHE_H

// Derived planes share the decoded-image budget. Keys contain the complete
// relevant state, not a hash: collisions cannot reuse the wrong picture.
typedef struct wc_cached_plane {
  struct wc_cached_plane *next;
  wc_image_t image;
  size_t key_bytes;
  unsigned kind;
  unsigned char key[];
} wc_cached_plane_t;

static void plane_evict(wc_t *c, wc_cached_plane_t *plane) {
  wc_cached_plane_t **link = &c->planes;
  while (*link && *link != plane)
    link = &(*link)->next;
  if (!*link)
    return;
  *link = plane->next;
  c->image_bytes -= plane->image.bytes;
  free(plane->image.pixels);
  free(plane);
  c->evictions++;
}

#if DG_D10
static const wc_image_t *plane_find(wc_t *c, unsigned kind, const void *key,
                                    size_t bytes) {
  for (wc_cached_plane_t *p = c->planes; p; p = p->next)
    if (p->kind == kind && p->key_bytes == bytes &&
        !memcmp(p->key, key, bytes)) {
      p->image.last = c->serial;
      c->plane_hits++;
      return &p->image;
    }
  return NULL;
}

// A cache is optional. If all admitted pictures are still in use, render the
// new plane normally and release it afterward instead of exceeding its budget.
static bool evict_oldest(wc_t *);
static const wc_image_t *plane_keep(wc_t *c, unsigned kind, const void *key,
                                    size_t bytes, size_t replace_prefix,
                                    const wc_image_t *image) {
  size_t pixels = (size_t)image->width * image->height * sizeof(*image->pixels);
  if (bytes > SIZE_MAX - sizeof(wc_cached_plane_t) ||
      pixels > SIZE_MAX - bytes - sizeof(wc_cached_plane_t))
    return NULL;
  size_t total = pixels + bytes + sizeof(wc_cached_plane_t);
  if (total > c->image_budget)
    return NULL;
  // Animated instances retain their latest surface, not every historical
  // revision. Mask pairs have no replacement prefix and remain ordinary LRU.
  for (wc_cached_plane_t *p = c->planes, *next; p; p = next) {
    next = p->next;
    if (replace_prefix && p->kind == kind && p->key_bytes >= replace_prefix &&
        !memcmp(p->key, key, replace_prefix))
      plane_evict(c, p);
  }
  while (c->image_bytes > c->image_budget - total && evict_oldest(c)) {
  }
  if (c->image_bytes > c->image_budget - total)
    return NULL;
  wc_cached_plane_t *p = malloc(sizeof(*p) + bytes);
  if (!p)
    return NULL;
  *p = (wc_cached_plane_t){
      .next = c->planes, .image = *image, .key_bytes = bytes, .kind = kind};
  p->image.last = c->serial;
  p->image.bytes = total;
  memcpy(p->key, key, bytes);
  c->planes = p;
  c->image_bytes += total;
  c->plane_builds++;
  return &p->image;
}
#endif
#endif
