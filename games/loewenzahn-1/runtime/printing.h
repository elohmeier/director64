#ifndef LOEWENZAHN_PRINTING_H
#define LOEWENZAHN_PRINTING_H
#include "director.h"
#include "input.h"

typedef struct {
  const char *title, *url;
  unsigned modules;
  uint8_t bits[(49 * 49 + 7) / 8];
} print_document_t;

void game_print_init(lv_runtime_t *);
const print_document_t *game_print_document(void);
unsigned game_print_id(void);
bool game_print_paused(void);
bool game_print_input(const input_sample_t *);
bool game_print_module(const print_document_t *, unsigned, unsigned);
#endif
