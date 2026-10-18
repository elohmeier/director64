#include "avm1_native.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t values;
static dg_avm1_library_t *library;
static dg_avm1_t *vm;
static unsigned callbacks, key_events, focus_events;
static lv_t expected_receiver;
static lv_t text(const char *s) { return lv_text(&values, s, false); }
static lv_t method(lv_t receiver, const char *name, unsigned argc,
                   const lv_t *args) {
  lv_t out = 0, fn = dg_avm1_get(vm, receiver, name);
  assert(dg_avm1_callable(vm, fn));
  bool ok = dg_avm1_invoke(vm, fn, receiver, argc, args, 16384, &out);
  if (!ok)
    fprintf(stderr, "native method failed: %s: %s\n", name, values.error);
  assert(ok);
  assert(!values.failed);
  return out;
}
static lv_t global(const char *name, unsigned argc, const lv_t *args) {
  return method(dg_avm1_globals(vm), name, argc, args);
}
static bool callback(void *ctx, dg_avm1_t *v, const char *name, lv_t receiver,
                     unsigned argc, const lv_t *args, lv_t *out) {
  (void)ctx;
  assert(v == vm);
  if (!strcmp(name, "test.watch")) {
    assert(receiver == expected_receiver && argc == 4);
    assert(!strcmp(lv_cstr(&values, args[0]), "position"));
    assert(lv_numeric(args[1]) == 10 && lv_numeric(args[2]) == 99);
    assert(lv_numeric(args[3]) == 7);
    callbacks++;
    // A watched assignment may modify the proposed value.
    *out = lv_num(42);
    return true;
  }
  if (!strcmp(name, "test.interval")) {
    assert(receiver == expected_receiver && argc == 1 &&
           lv_numeric(args[0]) == 123);
    callbacks++;
    *out = 0;
    return true;
  }
  if (!strcmp(name, "test.key")) {
    key_events++;
    *out = 0;
    return true;
  }
  if (!strcmp(name, "test.focus")) {
    assert(argc == 2);
    focus_events++;
    *out = 0;
    return true;
  }
  return false;
}
int main(void) {
  lv_init(&values, (lv_services_t){0}, NULL, NULL, 0, 1);
  library = dg_avm1_library_create((dg_avm1_host_t){.call = callback}, NULL);
  assert(library);
  vm = dg_avm1_create(&values, 7, dg_avm1_library_host(), library);
  assert(vm && dg_avm1_library_bind(library, vm));
  // Input without registered listeners is a normal empty broadcast.
  dg_avm1_library_key(library, 65, 97, true);
  dg_avm1_library_pointer(library, 7, 9, 1);
  dg_avm1_library_pointer(library, 7, 9, 0);
  assert(!values.failed);
  lv_t globals = dg_avm1_globals(vm), object = global("Object", 0, NULL);
  dg_avm1_set(vm, globals, "testObject", object);
  expected_receiver = object;
  dg_avm1_set(vm, object, "position", lv_num(10));
  lv_t watch_args[] = {text("position"), dg_avm1_native(vm, "test.watch"),
                       lv_num(7)};
  assert(dg_avm1_truth(vm, method(object, "watch", 3, watch_args)));
  dg_avm1_set(vm, object, "position", lv_num(99));
  assert(lv_numeric(dg_avm1_get(vm, object, "position")) == 42 &&
         callbacks == 1);
  lv_t watched_key = text("position");
  assert(dg_avm1_truth(vm, method(object, "unwatch", 1, &watched_key)));
  dg_avm1_set(vm, object, "position", lv_num(22));
  assert(lv_numeric(dg_avm1_get(vm, object, "position")) == 22 &&
         callbacks == 1);
  dg_avm1_set(vm, object, "tick", dg_avm1_native(vm, "test.interval"));
  lv_t interval_args[] = {object, text("tick"), lv_num(20), lv_num(123)};
  lv_t id = global("setInterval", 4, interval_args);
  dg_avm1_library_service(library, 19);
  assert(callbacks == 1);
  dg_avm1_library_service(library, 20);
  assert(callbacks == 2);
  // Repeated service without elapsed time cannot rerun the timer. A delayed
  // frame invokes once, retaining the bound receiver and trailing argument.
  dg_avm1_library_service(library, 20);
  assert(callbacks == 2);
  dg_avm1_library_service(library, 1000);
  assert(callbacks == 3);
  global("clearInterval", 1, &id);
  dg_avm1_library_service(library, 2000);
  assert(callbacks == 3);
  lv_t key = dg_avm1_get(vm, globals, "Key");
  dg_avm1_set(vm, object, "onKeyDown", dg_avm1_native(vm, "test.key"));
  method(key, "addListener", 1, &object);
  dg_avm1_library_key(library, 65, 97, true);
  assert(key_events == 1 && lv_numeric(method(key, "getCode", 0, NULL)) == 65);
  lv_t code = lv_num(65);
  assert(dg_avm1_truth(vm, method(key, "isDown", 1, &code)));
  dg_avm1_library_key(library, 65, 97, false);
  assert(!dg_avm1_truth(vm, method(key, "isDown", 1, &code)));
  method(key, "removeListener", 1, &object);
  dg_avm1_library_key(library, 65, 97, true);
  assert(key_events == 1);
  lv_t selection = dg_avm1_get(vm, globals, "Selection");
  dg_avm1_set(vm, object, "onSetFocus", dg_avm1_native(vm, "test.focus"));
  method(selection, "addListener", 1, &object);
  lv_t focus = global("Object", 0, NULL);
  dg_avm1_set(vm, globals, "focusObject", focus);
  dg_avm1_set(vm, focus, "_target", text("/field"));
  assert(dg_avm1_truth(vm, method(selection, "setFocus", 1, &focus)));
  assert(focus_events == 1 &&
         !strcmp(lv_cstr(&values, method(selection, "getFocus", 0, NULL)),
                 "/field"));
  lv_t input[] = {lv_num(10), lv_num(20), lv_num(30)};
  lv_t array = global("Array", 3, input);
  dg_avm1_set(vm, globals, "array", array);
  dg_avm1_set(vm, array, "5", lv_num(60));
  assert(lv_numeric(dg_avm1_get(vm, array, "length")) == 6);
  lv_t splice[] = {lv_num(1), lv_num(2), lv_num(90)};
  lv_t removed = method(array, "splice", 3, splice);
  assert(lv_numeric(dg_avm1_get(vm, removed, "length")) == 2);
  assert(lv_numeric(dg_avm1_get(vm, removed, "0")) == 20);
  assert(lv_numeric(dg_avm1_get(vm, array, "length")) == 5);
  assert(lv_numeric(dg_avm1_get(vm, array, "1")) == 90);
  assert(lv_numeric(dg_avm1_get(vm, array, "4")) == 60);
  dg_avm1_set(vm, array, "length", lv_num(2));
  assert(!dg_avm1_get(vm, array, "4"));
  assert(!strcmp(lv_cstr(&values, method(array, "join", 0, NULL)), "10,90"));
  lv_t math = dg_avm1_get(vm, globals, "Math"), negative = lv_num(-1.5);
  assert(lv_numeric(method(math, "round", 1, &negative)) == -1);
  lv_t missing = 0;
  assert(dg_avm1_truth(vm, global("isNaN", 1, &missing)));
  // String positions are UTF-16 units, never UTF-8 byte offsets. A split
  // surrogate must retain its numeric value for subsequent charCodeAt.
  lv_t german = text("Grüße😀!");
  dg_avm1_set(vm, globals, "unicode", german);
  assert(lv_numeric(dg_avm1_get(vm, german, "length")) == 8);
  lv_t position = lv_num(2);
  assert(lv_numeric(method(german, "charCodeAt", 1, &position)) == 0xfc);
  assert(
      !strcmp(lv_cstr(&values, method(german, "charAt", 1, &position)), "ü"));
  lv_t bounds[] = {lv_num(2), lv_num(5)};
  assert(
      !strcmp(lv_cstr(&values, method(german, "substring", 2, bounds)), "üße"));
  lv_t needle = text("!");
  assert(lv_numeric(method(german, "indexOf", 1, &needle)) == 7);
  position = lv_num(INFINITY);
  assert(isnan(lv_numeric(method(german, "charCodeAt", 1, &position))));
  lv_t whole[] = {lv_num(0), lv_num(INFINITY)};
  assert(!strcmp(lv_cstr(&values, method(german, "substring", 2, whole)),
                 "Grüße😀!"));
  position = lv_num(5);
  lv_t surrogate = method(german, "charAt", 1, &position);
  position = lv_num(0);
  assert(lv_numeric(method(surrogate, "charCodeAt", 1, &position)) == 0xd83d);
  assert(!strcmp(lv_cstr(&values, method(german, "toUpperCase", 0, NULL)),
                 "GRÜßE😀!"));
  assert(!strcmp(lv_cstr(&values, method(text("İ"), "toLowerCase", 0, NULL)), "i"));
  assert(!strcmp(lv_cstr(&values, method(text("ﬃ"), "toUpperCase", 0, NULL)), "ﬃ"));
  lv_t capitals = text("ÄÖÜ ΟΣ ΟΣΑ");
  assert(!strcmp(lv_cstr(&values, method(capitals, "toLowerCase", 0, NULL)),
                 "äöü οσ οσα"));
  lv_t property = text("length");
  assert(dg_avm1_truth(vm, method(array, "hasOwnProperty", 1, &property)));
  property = text("length");
  assert(
      !dg_avm1_truth(vm, method(array, "isPropertyEnumerable", 1, &property)));
  property = text("0");
  assert(
      dg_avm1_truth(vm, method(array, "isPropertyEnumerable", 1, &property)));
  lv_t field =
      dg_avm1_object(vm, dg_avm1_library_prototype(library, "TextField"));
  dg_avm1_set(vm, globals, "field", field);
  dg_avm1_set(vm, field, "text", text("Grüße😀!"));
  lv_t replace[] = {lv_num(2), lv_num(5), text("een")};
  method(field, "replaceText", 3, replace);
  assert(!strcmp(lv_cstr(&values, dg_avm1_get(vm, field, "text")), "Green😀!"));
  lv_t insertion = text("X");
  method(field, "replaceSel", 1, &insertion); // Unfocused fields do not edit.
  assert(!strcmp(lv_cstr(&values, dg_avm1_get(vm, field, "text")), "Green😀!"));
  method(selection, "setFocus", 1, &field);
  lv_t range[] = {lv_num(5), lv_num(7)};
  method(selection, "setSelection", 2, range);
  insertion = text("Ö");
  method(field, "replaceSel", 1, &insertion);
  assert(!strcmp(lv_cstr(&values, dg_avm1_get(vm, field, "text")), "GreenÖ!"));
  assert(lv_numeric(method(selection, "getBeginIndex", 0, NULL)) == 6);
  assert(lv_numeric(method(selection, "getCaretIndex", 0, NULL)) == 6);
  // A native library must keep callbacks/prototypes alive across collection.
  lv_collect(&values);
  assert(lv_numeric(method(array, "pop", 0, NULL)) == 90);
  dg_avm1_library_dispose(library);
  dg_avm1_dispose(vm);
  assert(!values.external_roots && !values.failed);
  puts("AVM1 native libraries: OK");
  return 0;
}
