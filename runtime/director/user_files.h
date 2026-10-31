#ifndef DIRECTOR64_USER_FILES_H
#define DIRECTOR64_USER_FILES_H
#include <stdbool.h>
#include <stdint.h>

// Player-selected files are capabilities in a bounded transient store. They
// never become unrestricted host paths or silently enter the game's save store.
enum {
  DG_USER_OPEN = 1,
  DG_USER_SAVE = 2,
  DG_USER_EXPORT = 3,
  DG_USER_PRINT = 4
};
typedef struct {
  unsigned id, kind;
  char title[160], name[128], filter[256];
  const uint8_t *data;
  unsigned size;
} dg_user_request_t;

const dg_user_request_t *dg_user_request(void);
// OPEN supplies original bytes plus optional host-decoded RGBA pixels. SAVE
// supplies the chosen name; EXPORT/PRINT completion uses dg_user_complete.
// Pixels use the compositor's RGBA8888 convention (red in the low byte).
bool dg_user_reply(unsigned id, const char *name, const uint8_t *data,
                   unsigned size, const uint32_t *pixels, unsigned width,
                   unsigned height);
bool dg_user_cancel(unsigned id);
bool dg_user_complete(unsigned id, bool success);
void dg_user_reset(void);

#endif
