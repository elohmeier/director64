#include "text_plain.h"
#include "print_ui.h"
#include "printing.h"
#include <libdragon.h>
#include <stdio.h>
#include <string.h>

void print_ui_draw(unsigned title_font, unsigned help_font) {
  const print_document_t *doc = game_print_document();
  if (!doc) return;
  rdpq_set_mode_fill(RGBA32(255, 255, 255, 255));
  rdpq_fill_rectangle(28, 22, 612, 458);
  unsigned scale = 300 / (doc->modules + 8);
  int left = (640 - (doc->modules + 8) * scale) / 2 + 4 * scale;
  int top = 79 + 4 * scale;
  rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
  for (unsigned y = 0; y < doc->modules; y++)
    for (unsigned x = 0; x < doc->modules; x++)
      if (game_print_module(doc, x, y))
        rdpq_fill_rectangle(left + x * scale, top + y * scale,
                            left + (x + 1) * scale, top + (y + 1) * scale);
  rdpq_set_mode_standard();
  rdpq_mode_alphacompare(1);
  char plain[320];
  rdpq_text_print(&(rdpq_textparms_t){.style_id=1, .width=548, .align=ALIGN_CENTER},
                  title_font, 46, 52, text_plain(doc->title, plain, sizeof(plain)));
  rdpq_text_print(&(rdpq_textparms_t){.style_id=1, .width=548, .align=ALIGN_CENTER},
                  help_font, 46, 74, "Mit dem Handy scannen und drucken");
  // A readable fallback in two lines; the complete URL is encoded in the QR.
  char address[160];
  snprintf(address, sizeof(address), "%s", doc->url);
  char *path = strstr(address, "://");
  path = path ? strchr(path + 3, '/') : NULL;
  if (path) {
    *path++ = 0;
    rdpq_text_print(&(rdpq_textparms_t){.style_id=1, .width=548, .align=ALIGN_CENTER},
                    help_font, 46, 394, text_plain(address, plain, sizeof(plain)));
    rdpq_text_printf(&(rdpq_textparms_t){.style_id=1, .width=548, .align=ALIGN_CENTER},
                     help_font, 46, 414, "/%s", text_plain(path, plain, sizeof(plain)));
  }
  rdpq_text_print(&(rdpq_textparms_t){.style_id=1, .width=548, .align=ALIGN_CENTER},
                  help_font, 46, 443, "B: Zurueck");
}
