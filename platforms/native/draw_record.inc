/* Working-set recorder, included by each game's tests/director_probe.c after
 * its `director` definition. When DIRECTOR64_DRAW_RECORD names a file, every
 * image the stage draws is counted per movie, asset and ink, exactly as the
 * console's render pass would ask for it (a visible sprite with opacity,
 * a bitmap or a film loop's current pose, inside the stage), and the counts
 * are appended to that file at exit as "movie<TAB>asset<TAB>ink<TAB>ticks"
 * lines. `director64 working-sets` drives the journey and fuzz episodes with
 * the variable set and folds the lines into the port's working-sets.json,
 * which the packer turns into one image bundle per scene.
 */
enum { DRAW_RECORD_SLOTS = 32768 };
static struct { char movie[32], asset[40]; unsigned ink; unsigned long ticks; } draw_record_rows[DRAW_RECORD_SLOTS];
static unsigned draw_record_used;
static bool draw_record_enabled, draw_record_registered;
static uint32_t draw_record_hash(const char *movie, const char *asset, unsigned ink) {
  uint32_t h = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)movie; *p; p++) h = (h ^ *p) * 16777619u;
  h = (h ^ 9u) * 16777619u;
  for (const unsigned char *p = (const unsigned char *)asset; *p; p++) h = (h ^ *p) * 16777619u;
  return (h ^ ink) * 16777619u;
}
static void draw_record_flush(void) {
  const char *path = getenv("DIRECTOR64_DRAW_RECORD");
  if (!path || !*path || !draw_record_used) return;
  FILE *out = fopen(path, "a");
  if (!out) return;
  for (unsigned i = 0; i < DRAW_RECORD_SLOTS; i++)
    if (draw_record_rows[i].ticks)
      fprintf(out, "%s\t%s\t%u\t%lu\n", draw_record_rows[i].movie, draw_record_rows[i].asset,
              draw_record_rows[i].ink, draw_record_rows[i].ticks);
  fclose(out);
  memset(draw_record_rows, 0, sizeof(draw_record_rows));
  draw_record_used = 0;
}
static void draw_record_one(const char *movie, const char *asset, unsigned ink) {
  uint32_t i = draw_record_hash(movie, asset, ink) % DRAW_RECORD_SLOTS;
  for (unsigned probe = 0; probe < DRAW_RECORD_SLOTS; probe++, i = (i + 1) % DRAW_RECORD_SLOTS) {
    if (!draw_record_rows[i].ticks) {
      if (draw_record_used + 1 >= DRAW_RECORD_SLOTS) { draw_record_flush(); draw_record_one(movie, asset, ink); return; }
      snprintf(draw_record_rows[i].movie, sizeof(draw_record_rows[i].movie), "%s", movie);
      snprintf(draw_record_rows[i].asset, sizeof(draw_record_rows[i].asset), "%s", asset);
      draw_record_rows[i].ink = ink;
      draw_record_rows[i].ticks = 1;
      draw_record_used++;
      return;
    }
    if (draw_record_rows[i].ink == ink && !strcmp(draw_record_rows[i].asset, asset) &&
        !strcmp(draw_record_rows[i].movie, movie)) {
      draw_record_rows[i].ticks++;
      return;
    }
  }
}
static void draw_record_tick(dg_runtime_t *d) {
  if (!draw_record_registered) {
    const char *path = getenv("DIRECTOR64_DRAW_RECORD");
    draw_record_enabled = path && *path;
    draw_record_registered = true;
    if (draw_record_enabled) atexit(draw_record_flush);
  }
  if (!draw_record_enabled || !d->movie) return;
  const uint16_t *order;
  unsigned count = dg_draw_order(d, &order);
  for (unsigned draw = 0; draw < count; draw++) {
    const dg_sprite_t *s = &d->sprites[order[draw]];
    if (!s->visible || !s->value.type || !dg_opacity(s)) continue;
    const dg_member_t *m = dg_member(d, s->value.member);
    if (!m || !m->width || !m->height) continue;
    const char *asset = NULL;
    if (m->type == 1) asset = m->asset;
    else if (m->type == 2 && m->film_count) asset = m->film_assets[dg_film_pose(m, s)];
    if (!asset || !*asset) continue;
    int l, t, r, b;
    dg_sprite_bounds(d, s, &l, &t, &r, &b);
    // Sprites parked far offstage are storage, not something the stage
    // draws; the authored stages in the corpus are at most 1024 wide.
    if (r <= 0 || b <= 0 || l >= 1024 || t >= 768 || r <= l || b <= t) continue;
    draw_record_one(d->movie->code->name, asset, s->value.ink);
  }
}
