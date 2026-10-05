#ifndef WILLY_INPUT_H
#define WILLY_INPUT_H
#include "director.h"
#include "input.h"
// False consumes this sample while a platform notice is displayed/dismissed.
bool willy_input(dg_runtime_t *, const input_sample_t *);
#endif
