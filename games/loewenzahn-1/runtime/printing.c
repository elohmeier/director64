#include "printing.h"
#include <math.h>
#include <string.h>

#include "print_documents.inc"
_Static_assert(sizeof(print_documents) / sizeof(*print_documents) == 16,
               "Löwenzahn print inventory");
static bool (*source_call)(lv_runtime_t *, const char *, unsigned,
                           const lv_t *, lv_t *, bool *);
static unsigned selected;
static bool opening, closing;

static bool print_call(lv_runtime_t *r, const char *name, unsigned argc,
                        const lv_t *args, lv_t *out, bool *yield) {
  if (strcmp(name, "director64_print"))
    return source_call && source_call(r, name, argc, args, out, yield);
  if (argc != 2) {
    lv_fail(r, "invalid print request");
    return true;
  }
  if (selected || closing) {
    // A second click while a page is open matches the original's rejected
    // reentrant print: alert and keep playing.
    lv_script_fail(r, "print already in progress");
    return true;
  }
  const char *book = lv_cstr(r, args[0]);
  double chapter = lv_number(r, args[1]);
  if ((strcmp(book, "BAS") && strcmp(book, "REZ")) ||
      !isfinite(chapter) || chapter < 1 || chapter > 8 || floor(chapter) != chapter) {
    // Printing from an unselected chapter errors in the original dialog too.
    lv_script_fail(r, "unknown print document");
    return true;
  }
  selected = (strcmp(book, "BAS") ? 8 : 0) + (unsigned)chapter;
  opening = true;
  *out = (lv_t){0};
  *yield = false;
  return true;
}

void game_print_init(lv_runtime_t *r) {
  selected = 0;
  opening = closing = false;
  if (r->services.call != print_call) source_call = r->services.call;
  r->services.call = print_call;
}
unsigned game_print_id(void) { return selected; }
const print_document_t *game_print_document(void) {
  return selected ? &print_documents[selected - 1] : NULL;
}
bool game_print_paused(void) { return selected || closing; }
bool game_print_input(const input_sample_t *sample) {
  if (!game_print_paused()) return false;
  if (!sample->connected) return true;
  bool neutral = !sample->buttons && sample->stick_x > -20 &&
                 sample->stick_x < 20 && sample->stick_y > -20 && sample->stick_y < 20;
  if (closing) {
    if (neutral) closing = false;
  } else if (opening) {
    if (neutral) opening = false;
  } else if (sample->buttons & INPUT_B) {
    selected = 0;
    closing = true;
  }
  return true; // Consume dismissal and the neutral sample before resuming.
}
bool game_print_module(const print_document_t *doc, unsigned x, unsigned y) {
  if (!doc || x >= doc->modules || y >= doc->modules) return false;
  unsigned bit = y * doc->modules + x;
  return !!(doc->bits[bit / 8] & (1u << (7 - bit % 8)));
}
