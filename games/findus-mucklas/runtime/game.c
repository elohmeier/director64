#include "game.h"
#include "archive.h"
#include <ctype.h>
#include <string.h>
static archive_t archive;
save_status_t game_save_load(save_backend_t b) { return archive_load(&archive, b); }
save_status_t game_save_status(void) { return archive.status; }
unsigned game_save_generation(void) { return archive.generation; }
bool game_save_migrated(void) { return false; }
bool game_read_file(const char *name, char *out, unsigned cap, unsigned *length) {
  if (archive_read(&archive, name, out, cap, length)) return true;
  // Fresh installation configuration, not progress imported from the disc.
  // This selected disc has no VI movie. The source gates its attic control
  // with Vind; enabling it would request an asset absent from the installation.
  static const char ini[] = "[CD]\rcdrom=d\r[Wind]\rVind=0\r[Screen]\rfullscreen=1\r";
  if (archive_file_id(name) != 8 || cap < sizeof(ini)) return false;
  memcpy(out, ini, sizeof(ini)); *length = sizeof(ini) - 1; return true;
}
bool game_write_file(const char *name, const char *data, unsigned length) {
  return archive_write(&archive, name, data, length);
}
const char *game_nth_file(void *ctx, const char *folder, int index) {
  (void)ctx;
  char lower[256]; unsigned n = 0;
  for (; folder[n] && n + 1 < sizeof(lower); n++) lower[n] = (char)tolower((unsigned char)folder[n]);
  lower[n] = 0;
  if (strstr(lower, "p3media")) return index == 1 ? "intro.dxr" : "";
  static const char *const files[] = {"player1.p3", "player2.p3", "player3.p3",
    "player4.p3", "player5.p3", "player6.p3", "data.p3", "moblemang.dxt"};
  return index >= 1 && index <= 8 ? files[index - 1] : "";
}
const char *game_long_date(void *ctx) {
  (void)ctx;
  return "";
}
