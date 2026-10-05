#ifndef DIRECTOR64_GAME_H
#define DIRECTOR64_GAME_H
#include "storage.h"
/* One adapter is linked per ROM. Source identities and entry movie come from game.toml. */
save_status_t game_save_load(save_backend_t);
save_status_t game_save_status(void);
unsigned game_save_generation(void);
bool game_save_migrated(void);
bool game_read_file(const char *, char *, unsigned, unsigned *);
bool game_write_file(const char *, const char *, unsigned);
const char *game_nth_file(void *, const char *, int);
const char *game_long_date(void *);
#endif
