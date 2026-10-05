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
  bool race = !strcmp(d->movie->code->name,"KR.DXR");
  bool ball = !strcmp(d->movie->code->name,"KB.DXR");
  if (race || ball) {
    const unsigned primary[] = {INPUT_C_UP,INPUT_C_RIGHT,INPUT_C_DOWN,INPUT_C_LEFT};
    const unsigned buttons[] = {INPUT_UP,INPUT_RIGHT,INPUT_DOWN,INPUT_LEFT};
    const unsigned arrows[] = {126,124,125,123};
    const unsigned arrow_chars[] = {30,29,31,28};
    const char *names[] = {"upp","hoger","ner","vanster"};
    lv_t bindings = {0};
    if (race) {
      // Use the live second driver's localization, exactly as KR's kotte does.
      int id = lv_global_id(d->values,"gobjektreferens");
      lv_t refs = id >= 0 ? d->values->globals[id] : (lv_t){0};
      if (lv_type(refs) == LV_PROPLIST) {
        lv_t drivers = lv_get(d->values,NULL,"kotte",refs);
        if (lv_type(drivers) == LV_LIST)
          for (unsigned n=1;n<=lv_count(d->values,drivers);n++) {
            lv_t driver=lv_at(d->values,drivers,n);
            if (lv_type(driver) == LV_INSTANCE &&
                lv_numeric(lv_get(d->values,NULL,"spelarnr",driver)) == 2)
              bindings=lv_get(d->values,NULL,"minatangenter",driver);
          }
      }
    }
    /* Steering reads its two ports directly: a race belongs to the players
     * sitting at pads one and two, not to whoever holds the pointer. */
    input_pad_t pad = input_sample_pad(sample,1);
    unsigned second = pad.connected ? pad.buttons : 0;
    if (pad.connected) {
      if(pad.stick_x>24)second|=INPUT_RIGHT;
      if(pad.stick_x< -24)second|=INPUT_LEFT;
      if(pad.stick_y>24)second|=INPUT_UP;
      if(pad.stick_y< -24)second|=INPUT_DOWN;
    }
    for (unsigned i=0;i<4;i++) {
      unsigned code=arrows[i], character=arrow_chars[i];
      if (sample->connected && (sample->buttons & primary[i])) {
        wanted[code>>5]|=UINT32_C(1)<<(code&31);chars[code]=character;
      }
      if (second & buttons[i]) {
        if (race) {
          // Until the second driver exists, never steer player one with pad 2.
          if (lv_type(bindings) != LV_PROPLIST) continue;
          lv_t key=lv_get(d->values,NULL,names[i],bindings);
          if (lv_type(key) == LV_NUMBER && lv_numeric(key) >= 0 && lv_numeric(key) < 128)
            code=(unsigned)lv_numeric(key);
          else if (lv_type(key) == LV_STRING && strlen(lv_cstr(d->values,key)) == 1) {
            character=(unsigned char)*lv_cstr(d->values,key);
            code=mucklas_key_code(character);
          } else continue;
        }
        wanted[code>>5]|=UINT32_C(1)<<(code&31);chars[code]=character;
      }
    }
    if(second&INPUT_A){wanted[49>>5]|=UINT32_C(1)<<(49&31);chars[49]=' ';}
  }
  for (unsigned code=0;code<128;code++) {
    uint32_t bit=UINT32_C(1)<<(code&31);
    if ((owned[code>>5]|wanted[code>>5])&bit)
      dg_key(d,code,chars[code],!!(wanted[code>>5]&bit));
  }
  memcpy(owned,wanted,sizeof(owned));
}
#endif
