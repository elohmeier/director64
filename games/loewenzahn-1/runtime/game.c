#include "game.h"
// The recovered game has no player save files. No other title's FlashRAM
// format is imported or written.
save_status_t game_save_load(save_backend_t backend) {
  (void)backend;
  return SAVE_BLANK;
}
save_status_t game_save_status(void) { return SAVE_BLANK; }
unsigned game_save_generation(void) { return 0; }
bool game_save_migrated(void) { return false; }
bool game_read_file(const char *name, char *data, unsigned cap,
                    unsigned *length) {
  (void)name;
  (void)data;
  (void)cap;
  (void)length;
  return false;
}
bool game_write_file(const char *name, const char *data, unsigned length) {
  (void)name;
  (void)data;
  (void)length;
  return false;
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
