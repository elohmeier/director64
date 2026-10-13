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
  if (archive_read(&archive, name, data, cap, length) && *length) return true;
  // The projector's settings with the developers' debug switches: no copy
  // check (optgraph.dll), no CD drive search, no display mode change. The
  // engine names the file after the application ("Projector.exe" here).
  if (named(name, "pf3.ini") || named(name, "projector.ini"))
    return constant("[Settings]\r\nSingleInstance=1\r\n\r\n[Sound]\r\nLowSpoolBufferMs=5000\r\n"
                    "HighSpoolBufferMs=3000\r\n\r\n[Debug]\r\nCheckCopyProtection=0\r\n"
                    "CheckCDInsert=0\r\nChangeScreenDim=0\r\n",
                    data, cap, length);
  // The flag file beside the projector marks the installed data layout.
  if (named(name, "cd1pf3.flg"))
    return constant(" ", data, cap, length);
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
// The string tables and room descriptions, shipped from the disc.
const char *game_data_folder(void *ctx) {
  (void)ctx;
  return "SPIEL/XML";
}
