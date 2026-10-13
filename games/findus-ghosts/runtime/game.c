#include "game.h"
#include <ctype.h>
#include <string.h>
#include "archive.h"
static archive_t archive;
save_status_t game_save_load(save_backend_t backend) { return archive_load(&archive, backend); }
save_status_t game_save_status(void) { return archive.status; }
unsigned game_save_generation(void) { return archive.generation; }
bool game_save_migrated(void) { return false; }
static bool constant(const char *text, char *data, unsigned cap, unsigned *length) {
  unsigned n = (unsigned)strlen(text);
  if (n > cap) return false;
  memcpy(data, text, n);
  *length = n;
  return true;
}
static bool named(const char *name, const char *expected) {
  for (; *name && *expected; name++, expected++)
    if (tolower((unsigned char)*name) != *expected) return false;
  return !*name && !*expected;
}
bool game_read_file(const char *name, char *data, unsigned cap, unsigned *length) {
  if (archive_read(&archive, name, data, cap, length)) return true;
  // The launcher's developer switch plays from the installed copy: it skips
  // the CD drive search and the optgraph.dll copy check.
  if (named(name, "pfstart.ini"))
    return constant("[custom]\r\nhdEnabled=1\r\ndebug=0\r\n", data, cap, length);
  // What the installer writes to the Windows folder: the score folder.
  if (named(name, "pfgame.ini"))
    return constant("[Pettersson]\r\ninstpfad=C:\\Findus\\Score\r\n", data, cap, length);
  return false;
}
bool game_write_file(const char *name, const char *data, unsigned length) {
  return archive_write(&archive, name, data, length);
}
const char *game_nth_file(void *ctx, const char *path, int n) {
  (void)ctx;
  // The launcher's installed-copy test lists the Score folder for pfScore.
  const char *slash = strrchr(path, '\\');
  return n == 1 && slash && named(slash + 1, "score") ? "pfScore.dat" : "";
}
const char *game_long_date(void *ctx) {
  (void)ctx;
  return "";
}
