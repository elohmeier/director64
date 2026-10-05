#ifndef DIRECTOR64_STORAGE_H
#define DIRECTOR64_STORAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*save_read_fn)(void *context, size_t offset, void *data, size_t length);
typedef bool (*save_write_fn)(void *context, size_t offset, const void *data, size_t length);
typedef struct { save_read_fn read; save_write_fn write; void *context; } save_backend_t;
typedef enum { SAVE_BLANK, SAVE_VALID, SAVE_CORRUPT, SAVE_IO_ERROR, SAVE_FULL } save_status_t;
#endif
