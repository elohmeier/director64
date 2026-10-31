#include "../../platforms/android/jni/cursor_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  host_cursor_t cursor;
  host_cursor_init(&cursor);
  assert(host_cursor_software(&cursor));
  host_cursor_request(&cursor,0);
  unsigned pending=cursor.sequence;
  host_cursor_request(&cursor,HOST_CURSOR_SOFTWARE);
  assert(!host_cursor_software(&cursor));
  assert(!host_cursor_ready(&cursor,pending,0));
  assert(host_cursor_ready(&cursor,cursor.sequence,HOST_CURSOR_SOFTWARE));
  assert(host_cursor_software(&cursor));
  // Native cursor cannot be enabled until the worker has removed canvas pixels.
  host_cursor_request(&cursor,0);
  assert(!host_cursor_software(&cursor) && cursor.changed);
  unsigned arrow=cursor.sequence;
  assert(host_cursor_ready(&cursor,arrow,0));
  host_cursor_request(&cursor,1);
  assert(!host_cursor_ready(&cursor,arrow,0));
  assert(host_cursor_ready(&cursor,cursor.sequence,1));
  // Authored cursor fallback waits until the UI has hidden its native icon.
  host_cursor_request(&cursor,HOST_CURSOR_SOFTWARE);
  assert(!host_cursor_software(&cursor));
  assert(!host_cursor_ready(&cursor,cursor.sequence,0));
  assert(host_cursor_ready(&cursor,cursor.sequence,HOST_CURSOR_SOFTWARE));
  assert(host_cursor_software(&cursor));
  // Entering a modal hides both stage cursor representations without game ticks.
  host_cursor_request(&cursor,HOST_CURSOR_HIDDEN);
  assert(!host_cursor_software(&cursor));
  assert(host_cursor_ready(&cursor,cursor.sequence,HOST_CURSOR_HIDDEN));
  host_cursor_request(&cursor,HOST_CURSOR_SOFTWARE);
  assert(!host_cursor_software(&cursor));
  assert(host_cursor_ready(&cursor,cursor.sequence,HOST_CURSOR_SOFTWARE));
  assert(host_cursor_software(&cursor));
  puts("Android cursor: native/software exclusion, stale acknowledgements and modal holds pass");
}
