#ifndef DIRECTOR64_AVM1_H
#define DIRECTOR64_AVM1_H
#include "lingo_runtime.h"

typedef struct dg_avm1 dg_avm1_t;
typedef struct {
  bool (*get)(void *, dg_avm1_t *, lv_t, const char *, lv_t *);
  bool (*set)(void *, dg_avm1_t *, lv_t, const char *, lv_t);
  bool (*call)(void *, dg_avm1_t *, const char *, lv_t, unsigned, const lv_t *,
               lv_t *);
  lv_t (*target)(void *, dg_avm1_t *, lv_t, const char *);
  void (*trace)(void *, const char *);
} dg_avm1_host_t;
typedef enum { DG_AVM1_ERROR, DG_AVM1_IDLE, DG_AVM1_BUSY } dg_avm1_status_t;

dg_avm1_t *dg_avm1_create(lv_runtime_t *, unsigned, dg_avm1_host_t, void *);
void dg_avm1_dispose(dg_avm1_t *);
lv_runtime_t *dg_avm1_values(dg_avm1_t *);
lv_t dg_avm1_globals(dg_avm1_t *);
lv_t dg_avm1_object(dg_avm1_t *, lv_t prototype);
bool dg_avm1_owns(dg_avm1_t *, lv_t);
// Resolve Director's case-insensitive external spelling to an authored key.
bool dg_avm1_external_key(dg_avm1_t *, lv_t, const char *, char *, unsigned);
lv_t dg_avm1_native(dg_avm1_t *, const char *);
lv_t dg_avm1_get(dg_avm1_t *, lv_t, const char *);
void dg_avm1_set(dg_avm1_t *, lv_t, const char *, lv_t);
bool dg_avm1_own_get(dg_avm1_t *, lv_t, const char *, lv_t *);
void dg_avm1_own_set(dg_avm1_t *, lv_t, const char *, lv_t);
lv_t dg_avm1_keys(dg_avm1_t *, lv_t, bool inherited);
bool dg_avm1_delete(dg_avm1_t *, lv_t, const char *);
// AVM1 property attributes: 1 dontEnum, 2 dontDelete, 4 readOnly.
// A NULL name applies to all existing own properties; raw own_set bypasses
// them.
void dg_avm1_flags(dg_avm1_t *, lv_t, const char *, unsigned set,
                   unsigned clear);
void dg_avm1_set_prototype(dg_avm1_t *, lv_t, lv_t);
lv_t dg_avm1_prototype(dg_avm1_t *, lv_t);
bool dg_avm1_callable(dg_avm1_t *, lv_t);
bool dg_avm1_truth(dg_avm1_t *, lv_t);
double dg_avm1_number(dg_avm1_t *, lv_t);
lv_t dg_avm1_string(dg_avm1_t *, lv_t);
lv_t dg_avm1_boolean(bool);
lv_t dg_avm1_null(void);
// Immutable bytecode must remain resident until the VM is disposed: nested
// functions retain slices and lexical scopes. A run preserves continuations
// when its instruction budget expires.
bool dg_avm1_start(dg_avm1_t *, lv_t, const uint8_t *, unsigned);
dg_avm1_status_t dg_avm1_run(dg_avm1_t *, unsigned);
lv_t dg_avm1_result(dg_avm1_t *);
// Native callbacks can invoke script callbacks synchronously with a hard
// instruction bound, preserving their enclosing AVM1 continuation.
bool dg_avm1_invoke(dg_avm1_t *, lv_t, lv_t, unsigned, const lv_t *, unsigned,
                    lv_t *);
#endif
