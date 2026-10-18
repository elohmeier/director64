#include "avm1.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint8_t data[8192];
  unsigned n;
} code_t;
static lv_runtime_t r;
static dg_avm1_t *vm;
static unsigned callbacks;
static void byte(code_t *c, unsigned n) {
  assert(c->n < sizeof(c->data));
  c->data[c->n++] = (uint8_t)n;
}
static void word(code_t *c, unsigned n) {
  byte(c, n);
  byte(c, n >> 8);
}
static void text(code_t *c, const char *s) {
  do {
    byte(c, (unsigned char)*s);
  } while (*s++);
}
static void action(code_t *c, unsigned op, const code_t *data) {
  byte(c, op);
  if (op >= 128) {
    word(c, data ? data->n : 0);
    if (data)
      for (unsigned i = 0; i < data->n; i++)
        byte(c, data->data[i]);
  }
}
static void string(code_t *c, const char *s) {
  code_t p = {0};
  byte(&p, 0);
  text(&p, s);
  action(c, 0x96, &p);
}
static void integer(code_t *c, int n) {
  code_t p = {0};
  byte(&p, 7);
  for (unsigned i = 0; i < 4; i++)
    byte(&p, (unsigned)n >> (8 * i));
  action(c, 0x96, &p);
}
static void scalar(code_t *c, unsigned type, unsigned n) {
  code_t p = {0};
  byte(&p, type);
  if (type == 4 || type == 5 || type == 8)
    byte(&p, n);
  action(c, 0x96, &p);
}
static void get(code_t *c, const char *s) {
  string(c, s);
  byte(c, 0x1c);
}
static void function(code_t *c, const char *name, const char *param,
                     const code_t *body) {
  code_t p = {0};
  text(&p, name);
  word(&p, param ? 1 : 0);
  if (param)
    text(&p, param);
  word(&p, body->n);
  action(c, 0x9b, &p);
  for (unsigned i = 0; i < body->n; i++)
    byte(c, body->data[i]);
}
static void pool(code_t *c, const char *s) {
  code_t p = {0};
  word(&p, 1);
  text(&p, s);
  action(c, 0x88, &p);
}
static bool host(void *ctx, dg_avm1_t *v, const char *name, lv_t receiver,
                 unsigned argc, const lv_t *args, lv_t *out) {
  (void)ctx;
  (void)receiver;
  if (!strcmp(name, "getTimer")) {
    *out = lv_num(1234);
    return true;
  }
  if (!strcmp(name, "callback")) {
    assert(argc == 1);
    callbacks++;
    lv_collect(&r);
    return dg_avm1_invoke(v, args[0], dg_avm1_globals(v), 0, NULL, 100, out);
  }
  return false;
}
static void boot(unsigned version) {
  dg_avm1_dispose(vm);
  lv_init(&r, (lv_services_t){0}, NULL, NULL, 0, 123);
  vm = dg_avm1_create(&r, version, (dg_avm1_host_t){.call = host}, NULL);
  assert(vm);
}
static lv_t run(const code_t *c, unsigned budget) {
  assert(dg_avm1_start(vm, dg_avm1_globals(vm), c->data, c->n));
  unsigned rounds = 0;
  dg_avm1_status_t s;
  do {
    s = dg_avm1_run(vm, budget);
    if (r.failed)
      fprintf(stderr, "%s\n", r.error);
    assert(s != DG_AVM1_ERROR);
    assert(++rounds < 10000);
  } while (s == DG_AVM1_BUSY);
  return dg_avm1_result(vm);
}
static void number(lv_t n, double expected) {
  assert(lv_type(n) == LV_NUMBER);
  assert(lv_numeric(n) == expected);
}
static void primitives(void) {
  boot(7);
  code_t c = {0};
  scalar(&c, 5, 1);
  integer(&c, 1);
  byte(&c, 0x66);
  byte(&c, 0x3e);
  lv_t out = run(&c, 1);
  assert(lv_type(out) == LV_AVM_BOOLEAN && !dg_avm1_truth(vm, out));
  c = (code_t){0};
  scalar(&c, 2, 0);
  scalar(&c, 3, 0);
  byte(&c, 0x49);
  byte(&c, 0x3e);
  assert(dg_avm1_truth(vm, run(&c, 1)));
  c = (code_t){0};
  string(&c, "sum=");
  integer(&c, 3);
  byte(&c, 0x47);
  byte(&c, 0x3e);
  assert(!strcmp(lv_cstr(&r, run(&c, 1)), "sum=3"));
  assert(isnan(dg_avm1_number(vm, 0)));
  assert(dg_avm1_number(vm, dg_avm1_null()) == 0);
  assert(!dg_avm1_truth(vm, lv_float(NAN)));
  c = (code_t){0};
  byte(&c, 0x34);
  byte(&c, 0x3e);
  number(run(&c, 1), 1234);
  c = (code_t){0};
  integer(&c, -1);
  integer(&c, 1);
  byte(&c, 0x65);
  byte(&c, 0x3e);
  number(run(&c, 1), 2147483647);
  boot(5);
  assert(dg_avm1_number(vm, 0) == 0);
  assert(!*lv_cstr(&r, dg_avm1_string(vm, 0)));
  c = (code_t){0};
  string(&c, "Gr\xfc\xdf\x80");
  byte(&c, 0x3e);
  assert(!strcmp(lv_cstr(&r, run(&c, 1)), "Gr\xc3\xbc\xc3\x9f\xe2\x82\xac"));
}
static void objects(void) {
  boot(6);
  lv_t g = dg_avm1_globals(vm), proto = dg_avm1_object(vm, 0),
       o = dg_avm1_object(vm, proto);
  dg_avm1_own_set(vm, g, "object", o);
  assert(dg_avm1_owns(vm, o));
  dg_avm1_set(vm, proto, "Inherited", lv_num(11));
  number(dg_avm1_get(vm, o, "INHERITED"), 11);
  dg_avm1_set(vm, o, "inherited", lv_num(12));
  dg_avm1_set(vm, o, "Keep", lv_num(7));
  dg_avm1_flags(vm, o, "KEEP", 7, 0);
  dg_avm1_set(vm, o, "keep", lv_num(99));
  number(dg_avm1_get(vm, o, "keep"), 7);
  assert(!dg_avm1_delete(vm, o, "Keep"));
  dg_avm1_flags(vm, o, "inherited", 1, 0);
  assert(!lv_count(&r, dg_avm1_keys(vm, o, true)));
  dg_avm1_flags(vm, o, NULL, 0, 7);
  assert(lv_count(&r, dg_avm1_keys(vm, o, true)) == 2);
  assert(dg_avm1_delete(vm, o, "keep"));
  lv_collect(&r);
  number(dg_avm1_get(vm, o, "Inherited"), 12);
  boot(7);
  g = dg_avm1_globals(vm);
  dg_avm1_set(vm, g, "Upper", lv_num(1));
  assert(!dg_avm1_get(vm, g, "upper"));
  char resolved[32];
  assert(dg_avm1_external_key(vm, g, "UPPER", resolved, sizeof(resolved)));
  assert(!strcmp(resolved, "Upper"));
  code_t c = {0};
  string(&c, "x");
  integer(&c, 3);
  byte(&c, 0x1d);
  string(&c, "y");
  integer(&c, 4);
  byte(&c, 0x1d);
  get(&c, "x");
  get(&c, "y");
  byte(&c, 0x0a);
  byte(&c, 0x3e);
  number(run(&c, 1), 7);
}
static void closures(void) {
  boot(7);
  code_t inner = {0}, factory = {0}, c = {0};
  get(&inner, "x");
  byte(&inner, 0x3e);
  function(&factory, "", NULL, &inner);
  byte(&factory, 0x3e);
  function(&c, "factory", "x", &factory);
  string(&c, "saved");
  integer(&c, 41);
  integer(&c, 1);
  string(&c, "factory");
  byte(&c, 0x3d);
  byte(&c, 0x1d);
  integer(&c, 0);
  string(&c, "saved");
  byte(&c, 0x3d);
  byte(&c, 0x3e);
  number(run(&c, 1), 41);
  lv_collect(&r);
  lv_t out = 0;
  assert(dg_avm1_invoke(vm, dg_avm1_get(vm, dg_avm1_globals(vm), "saved"),
                        dg_avm1_globals(vm), 0, NULL, 100, &out));
  number(out, 41);
  boot(7);
  c = (code_t){0};
  inner = (code_t){0};
  scalar(&inner, 8, 0);
  byte(&inner, 0x3e);
  pool(&c, "captured");
  function(&c, "oldPool", NULL, &inner);
  pool(&c, "new");
  integer(&c, 0);
  string(&c, "oldPool");
  byte(&c, 0x3d);
  byte(&c, 0x3e);
  assert(!strcmp(lv_cstr(&r, run(&c, 1)), "captured"));
}
static void constructors_registers(void) {
  boot(7);
  code_t body = {0}, c = {0};
  get(&body, "this");
  string(&body, "value");
  get(&body, "v");
  byte(&body, 0x4f);
  scalar(&body, 3, 0);
  byte(&body, 0x3e);
  function(&c, "C", "v", &body);
  integer(&c, 27);
  integer(&c, 1);
  string(&c, "C");
  byte(&c, 0x40);
  string(&c, "value");
  byte(&c, 0x4e);
  byte(&c, 0x3e);
  number(run(&c, 1), 27);
  boot(7);
  body = (code_t){0};
  c = (code_t){0};
  scalar(&body, 4, 1);
  string(&body, "offset");
  byte(&body, 0x4e);
  scalar(&body, 4, 2);
  byte(&body, 0x0a);
  byte(&body, 0x3e);
  code_t header = {0};
  text(&header, "method");
  word(&header, 1);
  byte(&header, 3);
  word(&header, 1 | 8 | 32);
  byte(&header, 2);
  text(&header, "p");
  word(&header, body.n);
  action(&c, 0x8e, &header);
  for (unsigned i = 0; i < body.n; i++)
    byte(&c, body.data[i]);
  byte(&c, 0);
  run(&c, 1);
  lv_t g = dg_avm1_globals(vm), o = dg_avm1_object(vm, 0);
  dg_avm1_own_set(vm, g, "o", o);
  dg_avm1_own_set(vm, o, "offset", lv_num(10));
  dg_avm1_own_set(vm, o, "f", dg_avm1_get(vm, g, "method"));
  // Keep the defining bytecode resident while this second action uses it.
  code_t call = {0};
  integer(&call, 9);
  integer(&call, 1);
  get(&call, "o");
  string(&call, "f");
  byte(&call, 0x52);
  byte(&call, 0x3e);
  number(run(&call, 1), 19);
}
static void continuation(void) {
  boot(7);
  code_t c = {0};
  string(&c, "x");
  integer(&c, 0);
  byte(&c, 0x1d);
  unsigned loop = c.n;
  string(&c, "x");
  get(&c, "x");
  byte(&c, 0x50);
  byte(&c, 0x1d);
  get(&c, "x");
  integer(&c, 25);
  byte(&c, 0x48);
  code_t jump = {0};
  int offset = (int)loop - (int)(c.n + 5);
  word(&jump, (unsigned)offset);
  action(&c, 0x9d, &jump);
  get(&c, "x");
  byte(&c, 0x3e);
  number(run(&c, 1), 25);
  boot(7);
  c = (code_t){0};
  code_t body = {0};
  string(&body, "survives collection");
  byte(&body, 0x3e);
  function(&c, "cb", NULL, &body);
  dg_avm1_own_set(vm, dg_avm1_globals(vm), "native",
                  dg_avm1_native(vm, "callback"));
  get(&c, "cb");
  integer(&c, 1);
  string(&c, "native");
  byte(&c, 0x3d);
  byte(&c, 0x3e);
  assert(!strcmp(lv_cstr(&r, run(&c, 1)), "survives collection"));
  assert(callbacks == 1);
}
static void bad(const uint8_t *code, unsigned count, const char *message) {
  boot(7);
  assert(dg_avm1_start(vm, dg_avm1_globals(vm), code, count));
  assert(dg_avm1_run(vm, 100) == DG_AVM1_ERROR);
  assert(strstr(r.error, message));
}
static void projector_observations(void) {
  // Original Adobe Windows standalone debugger 32,0,0,465 under Wine8.
  // SHA256 9d51df0c26e9ec4717a3da2e184cde831ca139d49e4a7c2f9e26028f8c4dfa52.
  // tools/reference/generate_avm1_probe.py reproduces all24 observations.
  for (unsigned version = 5; version <= 7; version++) {
    for (unsigned owner = 0; owner < 4; owner++) {
      boot(version);
      code_t c = {0};
      if (owner == 0)
        scalar(&c, 3, 0);
      else if (owner == 1)
        scalar(&c, 2, 0);
      else if (owner == 2)
        integer(&c, 3);
      else
        string(&c, "x");
      string(&c, "value");
      integer(&c, 17);
      byte(&c, 0x4f);
      string(&c, "after");
      byte(&c, 0x3e);
      assert(!strcmp(lv_cstr(&r, run(&c, 1)), "after"));
    }
    boot(version);
    code_t c = {0};
    integer(&c, 0);
    scalar(&c, 3, 0);
    string(&c, "missing");
    byte(&c, 0x52);
    byte(&c, 0x17);
    string(&c, "after");
    byte(&c, 0x3e);
    assert(!strcmp(lv_cstr(&r, run(&c, 1)), "after"));
    boot(version);
    c = (code_t){0};
    scalar(&c, 3, 0);
    byte(&c, 0x4a);
    byte(&c, 0x3e);
    lv_t result = run(&c, 1);
    if (version <= 6)
      number(result, 0);
    else
      assert(isnan(lv_numeric(result)));
    boot(version);
    c = (code_t){0};
    scalar(&c, 3, 0);
    byte(&c, 0x4b);
    byte(&c, 0x3e);
    assert(!strcmp(lv_cstr(&r, run(&c, 1)), version <= 6 ? "" : "undefined"));
    boot(version);
    c = (code_t){0};
    scalar(&c, 3, 0);
    string(&c, "x");
    byte(&c, 0x4e);
    byte(&c, 0x44);
    byte(&c, 0x3e);
    assert(!strcmp(lv_cstr(&r, run(&c, 1)), "undefined"));
  }
}
static void super_inheritance(void) {
  for (unsigned version = 6; version <= 7; version++) {
    boot(version);
    code_t c = {0}, base = {0}, child = {0}, grand = {0};
    get(&base, "this");
    string(&base, "value");
    byte(&base, 0x4e);
    byte(&base, 0x3e);
    integer(&child, 0);
    get(&child, "super");
    string(&child, "method");
    byte(&child, 0x52);
    integer(&child, 10);
    byte(&child, 0x47);
    byte(&child, 0x3e);
    integer(&grand, 0);
    get(&grand, "super");
    string(&grand, "method");
    byte(&grand, 0x52);
    integer(&grand, 20);
    byte(&grand, 0x47);
    byte(&grand, 0x3e);
    function(&c, "baseMethod", NULL, &base);
    function(&c, "childMethod", NULL, &child);
    function(&c, "grandMethod", NULL, &grand);
    byte(&c, 0);
    run(&c, 1);
    lv_t global = dg_avm1_globals(vm);
    lv_t bp = dg_avm1_object(vm, 0), cp = dg_avm1_object(vm, bp),
         gp = dg_avm1_object(vm, cp);
    dg_avm1_set(vm, bp, "method", dg_avm1_get(vm, global, "baseMethod"));
    dg_avm1_set(vm, cp, "method", dg_avm1_get(vm, global, "childMethod"));
    dg_avm1_set(vm, gp, "method", dg_avm1_get(vm, global, "grandMethod"));
    lv_t object = dg_avm1_object(vm, gp), out = 0;
    dg_avm1_set(vm, global, "receiver", object);
    dg_avm1_set(vm, object, "value", lv_num(7));
    assert(dg_avm1_invoke(vm, dg_avm1_get(vm, object, "method"), object, 0,
                          NULL, 200, &out));
    number(out, 37);
    // An inherited override starts super above its defining prototype, while
    // retaining the grandchild instance as this in the base method.
    dg_avm1_delete(vm, gp, "method");
    assert(dg_avm1_invoke(vm, dg_avm1_get(vm, object, "method"), object, 0,
                          NULL, 200, &out));
    number(out, version == 6 ? 27 : 17);

    code_t constructors = {0}, bc = {0}, cc = {0}, invoke = {0};
    get(&bc, "this");
    string(&bc, "value");
    get(&bc, "x");
    byte(&bc, 0x4f);
    byte(&bc, 0);
    get(&cc, "x");
    integer(&cc, 1);
    string(&cc, "super");
    byte(&cc, 0x3d);
    byte(&cc, 0x17);
    get(&cc, "this");
    string(&cc, "value");
    get(&cc, "this");
    string(&cc, "value");
    byte(&cc, 0x4e);
    integer(&cc, 3);
    byte(&cc, 0x47);
    byte(&cc, 0x4f);
    byte(&cc, 0);
    function(&constructors, "BaseCtor", "x", &bc);
    function(&constructors, "ChildCtor", "x", &cc);
    byte(&constructors, 0);
    run(&constructors, 1);
    lv_t bf = dg_avm1_get(vm, global, "BaseCtor"),
         cf = dg_avm1_get(vm, global, "ChildCtor");
    lv_t prototype = dg_avm1_object(vm, dg_avm1_get(vm, bf, "prototype"));
    dg_avm1_set(vm, cf, "prototype", prototype);
    integer(&invoke, 41);
    integer(&invoke, 1);
    get(&invoke, "ChildCtor");
    byte(&invoke, 0x40);
    byte(&invoke, 0x3e);
    number(dg_avm1_get(vm, run(&invoke, 1), "value"), 44);

    code_t functions = {0}, body = {0}, record = {0};
    integer(&body, 0);
    scalar(&body, 4, 1);
    string(&body, "method");
    byte(&body, 0x52);
    byte(&body, 0x3e);
    text(&record, "preloadedSuper");
    word(&record, 0);
    byte(&record, 2);
    word(&record, 16);
    word(&record, body.n);
    action(&functions, 0x8e, &record);
    for (unsigned i = 0; i < body.n; i++)
      byte(&functions, body.data[i]);
    byte(&functions, 0);
    run(&functions, 1);
    dg_avm1_set(vm, cp, "preloaded", dg_avm1_get(vm, global, "preloadedSuper"));
    assert(dg_avm1_invoke(vm, dg_avm1_get(vm, object, "preloaded"), object, 0,
                          NULL, 200, &out));
    number(out, version == 6 ? 17 : 7);
  }
}
int main(void) {
  primitives();
  objects();
  closures();
  constructors_registers();
  continuation();
  projector_observations();
  super_inheritance();
  const uint8_t underflow[] = {0x0a};
  bad(underflow, sizeof(underflow), "underflow");
  const uint8_t unknown[] = {0x7f};
  bad(unknown, sizeof(unknown), "unsupported opcode");
  const uint8_t truncated[] = {0x96, 10, 0, 0};
  bad(truncated, sizeof(truncated), "truncated");
  const uint8_t interior[] = {0x99, 2, 0, 1, 0, 0x96, 2, 0, 5, 1, 0};
  bad(interior, sizeof(interior), "boundary");
  const uint8_t infinite[] = {0x99, 2, 0, 0xfb, 0xff};
  boot(7);
  assert(dg_avm1_start(vm, dg_avm1_globals(vm), infinite, sizeof(infinite)));
  assert(dg_avm1_run(vm, 5) == DG_AVM1_BUSY);
  assert(dg_avm1_run(vm, 5) == DG_AVM1_BUSY);
  dg_avm1_dispose(vm);
  assert(!r.external_roots);
  puts(
      "AVM1: coercion, property attributes, closures, constructors, registers, "
      "pools, bounded continuations, callback GC and malformed code passed");
}
