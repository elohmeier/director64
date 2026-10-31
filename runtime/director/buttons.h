#ifndef DIRECTOR64_BUTTONS_H
#define DIRECTOR64_BUTTONS_H
#include "director.h"
#if DG_MODERN
// Native button controls use the type-specific MOVI shape/filled slots.
enum { DG_BUTTON_PUSH=1, DG_BUTTON_CHECK=2, DG_BUTTON_RADIO=3 };
unsigned dg_button_kind(const dg_member_t *member);
bool dg_button_hilite(dg_runtime_t *director,uint32_t member);
void dg_button_set_hilite(dg_runtime_t *director,uint32_t member,bool value);
#endif
#endif
