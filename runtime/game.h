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
/* The folder `the moviePath` names, or NULL for the drive root "C:\". The
   browser runtime links a weak NULL default; a port's engine that locates
   itself from its folder (Findus 8.5 Circus) answers its own. */
const char *game_movie_path(void *);
/* Whether a sprite is the field a port's own keyDown handlers type a name
   into (the runtime, a dg_runtime_t, and the sprite channel); the TV's
   keyboard then sends it characters. A weak false default is linked. */
bool game_key_field(void *, unsigned);
/* The shipped read-only data folder FileIO reads from ("SPIEL/XML"), or
   NULL; weak NULL default. */
const char *game_data_folder(void *);
#endif
