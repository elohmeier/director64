#include "avm1_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
  const char *name;
  unsigned version;
  const uint8_t *code;
  unsigned size;
  const char *expected;
} avm_observation_t;
#include "avm1_original.h"

static lv_runtime_t values;
int main(void) {
  for (unsigned i = 0; i < sizeof(observations) / sizeof(*observations); i++) {
    const avm_observation_t *o = &observations[i];
    lv_init(&values, (lv_services_t){0}, NULL, NULL, 0, 1);
    dg_avm1_library_t *library =
        dg_avm1_library_create((dg_avm1_host_t){0}, NULL);
    assert(library);
    dg_avm1_t *vm =
        dg_avm1_create(&values, o->version, dg_avm1_library_host(), library);
    assert(vm && dg_avm1_library_bind(library, vm));
    lv_t global = dg_avm1_globals(vm);
    assert(dg_avm1_start(vm, global, o->code, o->size));
    dg_avm1_status_t status;
    unsigned steps = 0;
    do {
      status = dg_avm1_run(vm, 1);
      if (status == DG_AVM1_ERROR)
        fprintf(stderr, "%s: %s\n", o->name, values.error);
      assert(status != DG_AVM1_ERROR && ++steps < 10000);
    } while (status == DG_AVM1_BUSY);
    lv_t report = dg_avm1_string(vm, dg_avm1_get(vm, global, "report"));
    const char *text = lv_cstr(&values, report);
    if (strcmp(text, o->expected))
      fprintf(stderr, "%s: actual '%s', original '%s'\n", o->name, text,
              o->expected);
    assert(!values.failed && !strcmp(text, o->expected));
    dg_avm1_library_dispose(library);
    dg_avm1_dispose(vm);
    assert(!values.external_roots);
  }
  puts("AVM1: 60 original-player coercion, Unicode and versioned super "
       "observations passed");
}
