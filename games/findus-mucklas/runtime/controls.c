#include <strings.h>
#include "controls.h"
#include <string.h>
#if DG_D8
unsigned mucklas_key_code(unsigned c) {
  static const uint8_t letters[] = {0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
  if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  if (c >= 'A' && c <= 'Z') return letters[c-'A'];
  return c == 8 ? 51 : c == 13 ? 36 : c == ' ' ? 49 : c == '-' ? 27 : 18;
}
bool mucklas_name_target(dg_runtime_t *d, unsigned sprite) {
  if (sprite != 39 || strcmp(d->movie->code->name,"LO.DXR")) return false;
  int id = lv_global_id(d->values,"lo_state");
  // Lingo text compares without case; the state is spelled as the script
  // that named it (#skriverNamn).
  return id >= 0 && !strcasecmp(lv_cstr(d->values,d->values->globals[id]),"skrivernamn");
}
void mucklas_pad_keys(dg_runtime_t *d, const input_sample_t *sample) {
  static uint32_t owned[4];
  uint32_t wanted[4] = {0};
  uint8_t chars[128] = {0};
  if (!strcmp(d->movie->code->name,"KR.DXR")) {
    const unsigned primary[] = {INPUT_C_UP,INPUT_C_RIGHT,INPUT_C_DOWN,INPUT_C_LEFT};
    const unsigned arrows[] = {126,124,125,123};
    const unsigned arrow_chars[] = {30,29,31,28};
    // The single player's race can use the pointer or these arrow keys.
    for (unsigned i=0;i<4;i++) {
      unsigned code=arrows[i], character=arrow_chars[i];
      if (sample->connected && (sample->buttons & primary[i])) {
        wanted[code>>5]|=UINT32_C(1)<<(code&31);chars[code]=character;
      }
    }
  }
  for (unsigned code=0;code<128;code++) {
    uint32_t bit=UINT32_C(1)<<(code&31);
    if ((owned[code>>5]|wanted[code>>5])&bit)
      dg_key(d,code,chars[code],!!(wanted[code>>5]&bit));
  }
  memcpy(owned,wanted,sizeof(owned));
}
#endif
