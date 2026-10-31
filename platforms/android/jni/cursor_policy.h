#ifndef DIRECTOR64_ANDROID_CURSOR_POLICY_H
#define DIRECTOR64_ANDROID_CURSOR_POLICY_H
#include <stdbool.h>
// Native icons appear only after a frame without the software cursor. Moving
// back to software waits for the UI to acknowledge hiding the native icon.
enum { HOST_CURSOR_SOFTWARE = -1, HOST_CURSOR_HIDDEN = 200 };
typedef struct {
  unsigned sequence, acknowledged_sequence;
  int requested, acknowledged;
  bool changed;
} host_cursor_t;
static inline void host_cursor_init(host_cursor_t *c) {
  *c=(host_cursor_t){.requested=HOST_CURSOR_SOFTWARE,.acknowledged=HOST_CURSOR_SOFTWARE};
}
static inline void host_cursor_request(host_cursor_t *c,int kind) {
  if(c->requested==kind)return;
  c->requested=kind;c->sequence++;c->changed=true;
}
static inline bool host_cursor_ready(host_cursor_t *c,unsigned sequence,int kind) {
  if(sequence!=c->sequence || kind!=c->requested)return false;
  c->acknowledged=kind;c->acknowledged_sequence=sequence;return true;
}
static inline bool host_cursor_software(const host_cursor_t *c) {
  return c->requested==HOST_CURSOR_SOFTWARE && c->acknowledged==HOST_CURSOR_SOFTWARE &&
      c->acknowledged_sequence==c->sequence;
}
#endif
