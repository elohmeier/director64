#ifndef DIRECTOR64_TEXT_PLAIN_H
#define DIRECTOR64_TEXT_PLAIN_H
#include <stddef.h>
// libdragon's rdpq_text_print and rdpq_text_printf parse "$xx" as a font
// change and "^xx" as a style change, and assert on anything else after
// the dollar. Text that came from the game or the player — a field, an
// alert, a notice, the name typed on the controller keyboard — must reach
// them with both doubled, which the parser reads as the literal character.
// The Deutsch showcase once died on a "$" the keyboard scenario typed.
// Returns `out`, truncated to what fits.
static inline const char *text_plain(const char *text, char *out, size_t size) {
  size_t at = 0;
  for (const unsigned char *p = (const unsigned char *)text; *p && at + 2 < size; p++) {
    if (*p == '$' || *p == '^') {
      if (at + 3 >= size) break;
      out[at++] = (char)*p;
    }
    out[at++] = (char)*p;
  }
  out[at] = 0;
  return out;
}
#endif
