#include "virtual_files.h"
const char *workshop_nth_file(void *ctx, const char *folder, int index) {
  (void)ctx; (void)folder;
  static const char *const names[] = {
    "vakt1.txt", "vakt2.txt", "vakt3.txt", "vakt4.txt", "vakt5.txt", "vakt6.txt", "vakt7.txt"
  };
  return index >= 1 && index <= 7 ? names[index-1] : "";
}
