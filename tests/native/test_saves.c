// The game's files (runtime/storage/saves.c): patterns, aliases, read-only
// files, folder listings, generations and a failed write.
#include "game.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t image[131072];
static bool fail_writes;
static bool image_read(void *ctx, size_t at, void *data, size_t n) {
  (void)ctx;
  memcpy(data, image + at, n);
  return true;
}
static bool image_write(void *ctx, size_t at, const void *data, size_t n) {
  (void)ctx;
  if (fail_writes) return false;
  memcpy(image + at, data, n);
  return true;
}
static const save_backend_t backend = {image_read, image_write, NULL};

int main(void) {
  char text[256];
  unsigned n;
  memset(image, 255, sizeof(image));
  assert(game_save_load(backend) == SAVE_BLANK);
  assert(!game_read_file("vakt1.txt", text, sizeof(text), &n));
  assert(game_write_file("VAKT1.TXT", "one", 3) && game_save_generation() == 1);
  assert(game_write_file("data.cst", "car", 3));
  assert(game_write_file("Löwenzahn 7.usr", "prefs", 5));
  assert(!game_write_file("other.txt", "x", 1));
  assert(game_read_file("vakt1.txt", text, sizeof(text), &n) && n == 3 && !strcmp(text, "one"));
  assert(game_read_file("DATA.CXT", text, sizeof(text), &n) && !strcmp(text, "car"));
  assert(!strcmp(game_save_name(0), "vakt1.txt") && !strcmp(game_save_name(1), "data.cxt"));
  assert(!strcmp(game_save_name(2), "löwenzahn 7.usr") && !game_save_name(3));
  assert(game_write_file("vakt1.txt", "first", 5) && game_save_generation() == 4);
  // Reloading finds the newest generation; a failed write keeps it.
  assert(game_save_load(backend) == SAVE_VALID && game_save_generation() == 4);
  fail_writes = true;
  assert(!game_write_file("vakt2.txt", "lost", 4));
  fail_writes = false;
  assert(game_save_load(backend) == SAVE_VALID && game_save_generation() == 4);
  assert(game_read_file("vakt1.txt", text, sizeof(text), &n) && !strcmp(text, "first"));
  assert(!game_read_file("vakt2.txt", text, sizeof(text), &n));
  // Read-only files answer what the installation had.
  assert(game_read_file("Projector.INI", text, sizeof(text), &n) && n == 12);
  assert(game_read_file("mulle.ico", text, sizeof(text), &n) && n == 0);
  assert(!game_write_file("pf3.ini", "x", 1));
  assert(!strcmp(game_nth_file(NULL, "C:\\P3MEDIA\\", 1), "intro.dxr"));
  assert(!strcmp(game_nth_file(NULL, "C:\\SAVES", 2), "b.p3"));
  assert(!strcmp(game_nth_file(NULL, "C:\\SAVES", 3), ""));
  assert(!strcmp(game_long_date(NULL), "Sunday, December 24, 2000"));
  // A damaged image is corrupt, not blank.
  image[40] ^= 1;
  image[65536 + 40] ^= 1;
  assert(game_save_load(backend) == SAVE_CORRUPT && !game_write_file("vakt1.txt", "x", 1));
  // An image of another format starts over.
  memset(image, 0x46, sizeof(image));
  assert(game_save_load(backend) == SAVE_BLANK && game_write_file("vakt1.txt", "x", 1));
  puts("saves: patterns, aliases, read-only files, listings and generations passed");
}
