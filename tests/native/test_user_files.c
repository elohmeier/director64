#include "director.h"
#include "user_files.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static bool yielded;
static lv_t invoke(const char *name, unsigned count, const lv_t *args) {
  lv_t result = 0;
  yielded = false;
  assert(values.services.call(&values, name, count, args, &result, &yielded));
  assert(!values.failed);
  return result;
}
static lv_t text(const char *s) { return lv_text(&values, s, false); }
static void wait(void) {
  assert(lv_run(&values, 32));
  assert(!values.depth);
}

int main(void) {
  dg_user_reset();
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  director.files[0].used = true;
  lv_t file = lv_make(LV_FILE, 0);
  invoke("setFilterMask", 2, (lv_t[]){file, text("Cars (*.car),*.car")});
  invoke("displayOpen", 1, &file);
  assert(yielded && values.depth == 1);
  const dg_user_request_t *request = dg_user_request();
  assert(request && request->kind == DG_USER_OPEN &&
         strstr(request->filter, "*.car"));
  unsigned id = request->id;
  unsigned ticks = director.ticks;
  uint64_t clock = director.clock;
  assert(dg_tick(&director, 0, 0, false, 32));
  assert(director.ticks == ticks && director.clock == clock);
  assert(lv_run(&values, 1) && values.depth == 1 && values.yielded);
  assert(!dg_user_reply(id + 1, "car.car", (const uint8_t *)"[#parts:[1]]", 12,
                        NULL, 0, 0));
  assert(!dg_user_reply(id, "../car.car", NULL, 0, NULL, 0, 0));
  static const char car[] = "[#parts:[1],#name:\"Car\"]";
  assert(dg_user_reply(id, "car.car", (const uint8_t *)car, sizeof(car) - 1,
                       NULL, 0, 0));
  assert(!dg_user_request());
  wait();
  assert(lv_type(values.result) == LV_STRING);
  lv_t path = values.result;
  assert(strstr(lv_cstr(&values, path), "C:\\Director64Files\\"));
  assert(lv_integer(&values, invoke("openFile", 3,
                                    (lv_t[]){file, path, lv_num(1)})) == 0);
  assert(lv_integer(&values, invoke("getLength", 1, &file)) == sizeof(car) - 1);
  lv_t read = invoke("readFile", 1, &file);
  assert(!strcmp(lv_cstr(&values, read), car));
  invoke("closeFile", 1, &file);
  assert(!yielded && !dg_user_request());

  invoke("displayOpen", 1, &file);
  id = dg_user_request()->id;
  assert(dg_user_cancel(id));
  wait();
  assert(lv_type(values.result) == LV_NUMBER &&
         lv_integer(&values, values.result) == 0);

  invoke("displaySave", 3, (lv_t[]){file, text("Save car"), text("car.car")});
  request = dg_user_request();
  assert(request->kind == DG_USER_SAVE);
  id = request->id;
  assert(dg_user_reply(id, "new.car", NULL, 0, NULL, 0, 0));
  wait();
  path = values.result;
  assert(lv_integer(&values, invoke("openFile", 3,
                                    (lv_t[]){file, path, lv_num(0)})) == -37);
  assert(lv_integer(&values, invoke("createFile", 2, (lv_t[]){file, path})) ==
         0);
  assert(lv_integer(&values, invoke("openFile", 3,
                                    (lv_t[]){file, path, lv_num(2)})) == 0);
  invoke("writeString", 2, (lv_t[]){file, text(car)});
  invoke("closeFile", 1, &file);
  request = dg_user_request();
  assert(yielded && request && request->kind == DG_USER_EXPORT &&
         request->id == id);
  assert(request->size == sizeof(car) - 1 &&
         !memcmp(request->data, car, sizeof(car) - 1));
  assert(!dg_user_complete(id + 1, true) && dg_user_complete(id, true));
  wait();
  assert(lv_integer(&values, values.result) == 0);
  assert(!dg_user_request());
  dg_user_reset();
  puts("Player file dialogs suspend, import exact bytes, cancel and export "
       "through isolated capabilities");
}
