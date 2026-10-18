#ifndef DIRECTOR64_AVM1_NATIVE_H
#define DIRECTOR64_AVM1_NATIVE_H
#include "avm1.h"

typedef struct dg_avm1_library dg_avm1_library_t;
// Allocate the library first, pass host()/library as the VM host/context,
// then bind the resulting VM. Downstream hooks implement display objects.
dg_avm1_library_t *dg_avm1_library_create(dg_avm1_host_t, void *);
dg_avm1_host_t dg_avm1_library_host(void);
bool dg_avm1_library_bind(dg_avm1_library_t *, dg_avm1_t *);
void dg_avm1_library_dispose(dg_avm1_library_t *);
lv_t dg_avm1_library_prototype(dg_avm1_library_t *, const char *);
// Clock values are the owning Flash movie's monotonic service milliseconds.
// Each due interval runs at most once per service call; delays do not create
// an unbounded catch-up loop. Callback execution has a hard action budget.
void dg_avm1_library_service(dg_avm1_library_t *, uint64_t);
void dg_avm1_library_key(dg_avm1_library_t *, unsigned, unsigned, bool);
void dg_avm1_library_pointer(dg_avm1_library_t *, int, int, unsigned);
bool dg_avm1_library_event(dg_avm1_library_t *, lv_t, const char *, unsigned,
                           const lv_t *);
// Object.registerClass bindings used by attachMovie/export instantiation.
lv_t dg_avm1_library_class(dg_avm1_library_t *, const char *);
#endif
