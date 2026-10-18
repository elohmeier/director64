#ifndef DIRECTOR64_FLASH_BRIDGE_H
#define DIRECTOR64_FLASH_BRIDGE_H
#include "director.h"
// Returns false for another URL scheme. A recognized Lingo URL either
// executes the bounded statement or reports the unsupported expression.
bool dg_flash_lingo_url(dg_runtime_t *, const char *);
#endif
