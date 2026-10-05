#include "game.h"
#include "archive.h"
#include "virtual_files.h"
static archive_t archive;
save_status_t game_save_load(save_backend_t b) { return archive_load(&archive, b); }
save_status_t game_save_status(void) { return archive.status; }
unsigned game_save_generation(void) { return archive.generation; }
bool game_save_migrated(void) { return archive.migrated; }
bool game_read_file(const char *n, char *out, unsigned cap, unsigned *length) {
  return archive_read(&archive, n, out, cap, length);
}
bool game_write_file(const char *n, const char *data, unsigned length) {
  return archive_write(&archive, n, data, length);
}
const char *game_nth_file(void *ctx, const char *folder, int index) {
  return workshop_nth_file(ctx, folder, index);
}
const char *game_long_date(void *ctx) {
  (void)ctx;
  return "";
}
