#ifndef DIRECTOR64_ARCHIVE_H
#define DIRECTOR64_ARCHIVE_H
#include "save.h"

#define ARCHIVE_FILES 9u
#define ARCHIVE_BYTES 65536u
#define ARCHIVE_HEADER 128u
#define ARCHIVE_FILE_LIMIT 16383u

// Two independently erasable FlashRAM generations. File order is fixed; no
// saved path is ever interpreted as a host/ROM filesystem path.
typedef struct {
  save_backend_t backend;
  save_status_t status;
  int active_slot;
  uint32_t generation, lengths[ARCHIVE_FILES];
  bool migrated;
  uint8_t data[ARCHIVE_BYTES - ARCHIVE_HEADER];
} archive_t;

int archive_file_id(const char *name);
save_status_t archive_load(archive_t *, save_backend_t);
bool archive_read(archive_t *, const char *, char *, unsigned, unsigned *);
bool archive_write(archive_t *, const char *, const char *, unsigned);
#endif
