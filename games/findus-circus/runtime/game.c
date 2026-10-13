#include "game.h"
#include <string.h>
#include <strings.h>
#include "director.h"
#include "archive.h"
static archive_t archive;
save_status_t game_save_load(save_backend_t backend) { return archive_load(&archive, backend); }
save_status_t game_save_status(void) { return archive.status; }
unsigned game_save_generation(void) { return archive.generation; }
bool game_save_migrated(void) { return false; }
bool game_read_file(const char *name, char *data, unsigned cap, unsigned *length) {
  return archive_read(&archive, name, data, cap, length);
}
bool game_write_file(const char *name, const char *data, unsigned length) {
  return archive_write(&archive, name, data, length);
}
const char *game_nth_file(void *ctx, const char *path, int n) {
  (void)ctx;
  (void)path;
  (void)n;
  return "";
}
const char *game_long_date(void *ctx) {
  (void)ctx;
  return "";
}
// The engine locates itself from the projector's folder on the disc
// (ENGINE.CXT psPathLocator): a projector in PF2\ beside the movie file
// is its developer layout, which needs no installation or registry.
const char *game_movie_path(void *ctx) {
  (void)ctx;
  return "C:\\PF2\\";
}
// The player's name: LOGIN's own keyDown handlers type it into the editText
// field while the score waits at markers 50-52 or 64-66 (frames 105-114 and
// 175-184), so the TV's keyboard sends that field characters.
bool game_key_field(void *runtime, unsigned sprite) {
  dg_runtime_t *d = runtime;
  if (!d->movie || strcasecmp(d->movie->code->name, "LOGIN.DXR")) return false;
  if (!((d->frame >= 105 && d->frame < 115) || (d->frame >= 175 && d->frame < 185))) return false;
  const dg_member_t *m = dg_member(d, d->sprites[sprite].value.member);
  return m && m->name && !strcasecmp(m->name, "editText");
}
