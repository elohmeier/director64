#include "game.h"
#include <ctype.h>
#include <string.h>
#include "archive.h"
static archive_t archive;
save_status_t game_save_load(save_backend_t backend) { return archive_load(&archive, backend); }
save_status_t game_save_status(void) { return archive.status; }
unsigned game_save_generation(void) { return archive.generation; }
bool game_save_migrated(void) { return false; }
bool game_read_file(const char *name, char *data, unsigned cap,
                    unsigned *length) {
  if (archive_read(&archive, name, data, cap, length)) return true;
  // The embedded launcher opens this disc marker only to check existence.
  // Its actual icon is present in the selected, hash-verified installation.
  char lower[32];
  unsigned i = 0;
  for (; name[i] && i + 1 < sizeof(lower); i++) lower[i] = (char)tolower((unsigned char)name[i]);
  lower[i] = 0;
  if (!strcmp(lower, "mulle.ico") && cap) {
    data[0] = 0; *length = 0; return true;
  }
  return false;
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
