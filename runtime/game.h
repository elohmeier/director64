#ifndef DIRECTOR64_GAME_H
#define DIRECTOR64_GAME_H
#include "storage.h"
/* The game's files (runtime/storage/saves.c), configured by its game.toml
   [player] table through the generated game_profile.h. */
save_status_t game_save_load(save_backend_t);
save_status_t game_save_status(void);
unsigned game_save_generation(void);
const char *game_save_name(unsigned);
bool game_read_file(const char *, char *, unsigned, unsigned *);
bool game_write_file(const char *, const char *, unsigned);
const char *game_nth_file(void *, const char *, int);
bool game_directory(void *, const char *, bool);
const char *game_long_date(void *);
#endif
