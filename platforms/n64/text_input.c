#include "text_plain.h"
#include "text_input.h"
#include <string.h>
#include <stdio.h>
#if DG_D8
#include "controls.h"
#endif
static struct { bool active; unsigned sprite, cell; uint16_t held; char text[21];
  bool key_down; unsigned key_code;
} keyboard;
#if DG_D8
static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ -1";
#define KEY_CELLS sizeof(keys)
#elif DG_D10
// German names and spelling answers need umlauts; the field text is
// windows-1252, so the key values carry those bytes directly.
static const char keys[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ\xC4\xD6\xDC\xDF"
    "0123456789 -_.";
#define KEY_CELLS (sizeof(keys)-1)
#else
static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
#define KEY_CELLS (sizeof(keys)-1)
#endif
// The rdpq text path expects UTF-8; the game's fields hold windows-1252.
// Latin letter bytes convert one-to-one onto their Unicode codepoints.
static unsigned utf8_append(char *out, unsigned at, unsigned char c) {
  if (c < 128) {
    out[at++] = (char)c;
  } else {
    out[at++] = (char)(0xC0 | (c >> 6));
    out[at++] = (char)(0x80 | (c & 63));
  }
  out[at] = 0;
  return at;
}
bool text_input_active(void) { return keyboard.active; }
bool text_input_open(dg_runtime_t *d, unsigned sprite, uint16_t held) {
  if (d->values->depth || !sprite || sprite>=DG_SPRITES) return false;
  const dg_member_t *m=dg_member(d,d->sprites[sprite].value.member);
  if (!m || (m->type!=3 && m->type!=7)) return false;
#if DG_D8
  if (!mucklas_name_target(d,sprite)) return false;
#else
  if (!dg_text_editable(d,sprite) || d->text_pending) return false;
#endif
  lv_t value=lv_get(d->values,NULL,"text",lv_make(LV_MEMBER, (int32_t)m->id));
  const char *text=lv_cstr(d->values,value);
  if(strlen(text)>20)return false;
  keyboard.sprite=sprite;keyboard.cell=0;keyboard.held=held;
  strcpy(keyboard.text,text);
  if(!strcmp(keyboard.text," "))keyboard.text[0]=0;
  keyboard.key_down=false;keyboard.active=true;return true;
}
void text_input_update(dg_runtime_t *d,const input_sample_t *sample) {
  uint16_t buttons=sample->buttons;
  if(sample->stick_x>40)buttons|=INPUT_RIGHT;
  if(sample->stick_x< -40)buttons|=INPUT_LEFT;
  if(sample->stick_y>40)buttons|=INPUT_UP;
  if(sample->stick_y< -40)buttons|=INPUT_DOWN;
  uint16_t pressed=buttons & ~keyboard.held;keyboard.held=buttons;
  if(pressed&INPUT_LEFT)keyboard.cell=(keyboard.cell/10)*10+(keyboard.cell+9)%10;
  if(pressed&INPUT_RIGHT)keyboard.cell=(keyboard.cell/10)*10+(keyboard.cell+1)%10;
  if(pressed&INPUT_UP)keyboard.cell=(keyboard.cell+KEY_CELLS-10)%KEY_CELLS;
  if(pressed&INPUT_DOWN)keyboard.cell=(keyboard.cell+10)%KEY_CELLS;
  bool finish=pressed&INPUT_START;
#if DG_D8
  finish |= ((pressed&INPUT_A)&&!keys[keyboard.cell]) || !mucklas_name_target(d,keyboard.sprite);
  if (keyboard.key_down && (!(buttons&(INPUT_A|INPUT_B)) || finish)) {
    dg_key(d,keyboard.key_code,0,false);keyboard.key_down=false;
  }
  if (finish) {keyboard.active=false;return;}
  if (!keyboard.key_down && (pressed&(INPUT_A|INPUT_B))) {
    unsigned character=pressed&INPUT_B ? 8 : (unsigned char)keys[keyboard.cell];
    keyboard.key_code=mucklas_key_code(character);keyboard.key_down=true;
    dg_key(d,keyboard.key_code,character,true);
  }
  const dg_member_t *m=dg_member(d,d->sprites[keyboard.sprite].value.member);
  if (m) {
    lv_t text=lv_get(d->values,NULL,"text",lv_make(LV_MEMBER, (int32_t)m->id));
    snprintf(keyboard.text,sizeof(keyboard.text),"%s",lv_cstr(d->values,text));
  }
#else
  unsigned length=strlen(keyboard.text);
  if((pressed&INPUT_B)&&length)keyboard.text[--length]=0;
  if((pressed&INPUT_A)&&length<20){keyboard.text[length++]=keys[keyboard.cell];keyboard.text[length]=0;}
  if(finish){
    if (dg_edit_text(d,keyboard.sprite,keyboard.text)) keyboard.active=false;
  }
#endif
}
void text_input_draw(unsigned font_id, unsigned help_id) {
  if(!keyboard.active)return;
  rdpq_sync_pipe();rdpq_set_mode_fill(RGBA32(32,42,52,255));rdpq_fill_rectangle(90,90,550,390);
  rdpq_set_mode_standard();rdpq_mode_alphacompare(1);
  rdpq_text_print(&(rdpq_textparms_t){.style_id=1},font_id,110,120,"Name eingeben");
  char shown[sizeof(keyboard.text) * 2 + 2];
  unsigned at = 0;
  for (const char *p = keyboard.text; *p; p++)
    at = utf8_append(shown, at, (unsigned char)*p);
  char plain[sizeof(shown) * 2];
  rdpq_text_printf(&(rdpq_textparms_t){.style_id=1},font_id,110,150,"%s_",
                   text_plain(shown, plain, sizeof(plain)));
  for(unsigned i=0;i<KEY_CELLS;i++) {
    int x=112+(i%10)*42,y=180+(i/10)*38;
    if(i==keyboard.cell){rdpq_set_mode_fill(RGBA32(80,120,155,255));rdpq_fill_rectangle(x-6,y-20,x+24,y+9);rdpq_set_mode_standard();rdpq_mode_alphacompare(1);}
    char key[4], label[8];
    utf8_append(key, 0, (unsigned char)keys[i]);
    rdpq_text_print(&(rdpq_textparms_t){.style_id=1},font_id,x,y,
                    keys[i]?text_plain(key, label, sizeof(label)):"OK");
  }
  rdpq_text_print(&(rdpq_textparms_t){.style_id=1},help_id,110,355,"A: Buchstabe  B: Loeschen  Start: Fertig");
}
