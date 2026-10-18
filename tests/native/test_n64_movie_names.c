#include "movie_names.h"
#include <assert.h>
int main(void) {
  static const char *const names[] = {
      "utilitie_cxt",  "utilitie_dxr",       "scene_name_dxr",
      "utilities_dxr", "room__intro_dxr",    "other__intro_dxr",
      "introdac_dxr",  "room__baumhoeh_dxr", "simple_dxr"};
  char alias[40];
#define LOOKUP(n, p, c, count)                                                 \
  n64_movie_lookup(n, p, c, names, count, alias, sizeof(alias))
  assert(LOOKUP("Utilities", "", "", 3) == 2);
  assert(!strcmp(alias, "Utilities.dxr"));
  assert(LOOKUP("Utilities", "", "", 9) == 4 && !alias[0]);
  assert(LOOKUP("utilities.cxt", "", "", 9) == 1);
  assert(LOOKUP("Scene_Name.dir", "", "", 9) == 3 && !alias[0]);
  assert(LOOKUP("Scene_Name", "", "", 9) == 3);
  assert(LOOKUP("Intro", "@:room:intro", "OTHER__START.DXR", 9) == 5);
  assert(!strcmp(alias, "Intro.dxr"));
  assert(LOOKUP("Intro", "", "OTHER__START.DXR", 9) == 6);
  assert(LOOKUP("Intro_Dach", "", "", 9) == 7);
  assert(LOOKUP("Baumhoehle.DCR", "@:room:Baumhoehle.DCR", "", 9) == 8);
  assert(LOOKUP("simple", "@:room:simple", "OTHER__START.DXR", 9) == 9 &&
         !alias[0]);
  assert(!LOOKUP("missing", "", "", 9) && !alias[0]);
  char long_name[256];
  memset(long_name, 'x', 255);
  long_name[255] = 0;
  assert(!LOOKUP(long_name, "", "", 9));
  puts("N64 movie names preserve exact, folder-qualified and ISO9660 aliases");
}
