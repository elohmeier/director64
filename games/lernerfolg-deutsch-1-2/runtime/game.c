#include "game.h"
#include <string.h>
#include "archive.h"
static archive_t archive;
save_status_t game_save_load(save_backend_t backend) { return archive_load(&archive, backend); }
save_status_t game_save_status(void) { return archive.status; }
unsigned game_save_generation(void) { return archive.generation; }
bool game_save_migrated(void) { return false; }
bool game_read_file(const char *name, char *data, unsigned cap,
                    unsigned *length) {
  // Preference and profile files come from FlashRAM; the read-only exercise
  // databases ship on the ROM filesystem and remain platform reads.
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
  // Save timestamps display a fixed date until an RTC selection exists.
  return "Montag, 1. September 2014";
}
