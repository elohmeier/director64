#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t director;
static lv_runtime_t values;
static const lv_handler_t handler = {.name = "test", .kind = "MovieScript"};
static const lv_movie_t code = {.name = "XML.DXR", .count = 1,
                                .handlers = &handler};

static lv_t call(const char *name, unsigned count, const lv_t *args) {
  lv_frame_t frame = {.movie = &code, .handler = &handler};
  lv_t result = lv_call(&values, &frame, name, count, args);
  assert(!values.failed && !values.script_error_count);
  return result;
}

static int selected_state(const char *xml, bool has_alias) {
  lv_t name = lv_text(&values, "xmlParser", false);
  lv_t xtra = call("xtra", 1, &name);
  lv_t parser = call("new", 1, &xtra);
  call("parsestring", 2,
       (lv_t[]){parser, lv_text(&values, xml, false)});
  lv_t children = lv_get(&values, NULL, "child", parser);
  assert(lv_count(&values, children) == 1);
  lv_t attributes = lv_get(&values, NULL, "attributevalue",
                           lv_at(&values, children, 1));
  lv_t id_text = lv_index_get(&values, attributes,
                              lv_text(&values, "id", false));
  lv_t alias_text = lv_index_get(&values, attributes,
                                 lv_text(&values, "aliasID", false));
  lv_t id = call("integer", 1, &id_text);
  lv_t alias = call("integer", 1, &alias_text);
  assert((lv_type(alias) != LV_VOID) == has_alias);
  // Optional aliases select a different state only when explicitly supplied.
  // Converting an absent attribute to zero used to redirect every state.
  return lv_integer(&values, lv_type(alias) == LV_VOID ? id : alias);
}

int main(void) {
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 42);
  assert(selected_state("<state id='7'/>", false) == 7);
  assert(selected_state("<state id='7' aliasID='0'/>", true) == 0);
  assert(selected_state("<state id='7' aliasID='3'/>", true) == 3);
  assert(selected_state("<state id='7' aliasID=''/>", false) == 7);
  puts("XML optional numeric aliases: ok");
}
