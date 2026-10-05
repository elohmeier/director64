#ifndef DIRECTOR64_PACKAGE_H
#define DIRECTOR64_PACKAGE_H
// Loads a game package (docs/package-format.md) into the structures the
// generated C would otherwise link: every movie's handler table and scene
// tables, the corpus symbols and the global slots. Built with
// DIRECTOR64_PACKAGE, which is also what makes the runtime's symbol table
// loadable.
#include "director.h"
#include <stddef.h>
#include <stdint.h>

#define DG_PACKAGE_FORMAT 2
#define DG_PACKAGE_ABI_BYTES 16

typedef struct {
  uint8_t *bytes; // the package's own copy; strings and code point into it
  size_t length;
  const dg_movie_t **registry;
  unsigned movie_count;
  // Each movie's name and double pool lengths, two per registry entry: the
  // runtime needs neither (the code's indices are verified), loader parity
  // compares the pools with them.
  unsigned *pool_counts;
  const char **globals;
  unsigned global_count;
  lb_symbol_table_t symbols;
  // Every allocation, released together.
  void **blocks;
  unsigned block_count, block_capacity;
} dg_package_t;

// Validates and loads a package against the runtime's ABI digest. On failure
// nothing stays allocated and `error` names the first check that failed.
bool dg_package_load(dg_package_t *, const uint8_t *data, size_t length,
                     const uint8_t abi[DG_PACKAGE_ABI_BYTES], char *error,
                     size_t error_size);
void dg_package_free(dg_package_t *);
// Makes a loaded package's symbols the runtime's (lingo_runtime.c).
void lv_use_symbols(lb_symbol_table_t);
#endif
