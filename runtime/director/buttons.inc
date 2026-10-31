#include "buttons.h"
// Separate from field slots: checkbox state survives text edits, casts and
// window context switches, without consuming the bounded mutable field table.
#define DG_BUTTON_STATE_ROOT (LV_ROOTS - 2)
_Static_assert(DG_BEHAVIOR_ROOT+DG_SPRITES < DG_BUTTON_STATE_ROOT,"button roots must be independent");
#if DG_D10
_Static_assert(DG_HELD_RESULT_ROOT < DG_BUTTON_STATE_ROOT,"button roots must be independent");
#endif
unsigned dg_button_kind(const dg_member_t *m) {
  return m && m->type==7 && m->shape>=DG_BUTTON_PUSH && m->shape<=DG_BUTTON_RADIO?m->shape:0;
}
static unsigned button_slot(dg_runtime_t *d,uint32_t member) {
  lv_runtime_t *r=d->values;lv_t state=r->roots[DG_BUTTON_STATE_ROOT];
  if(lv_type(state)!=LV_PROPLIST)return 0;
  for(unsigned i=1;i<=lv_count(r,state);i++)
    if((uint32_t)lv_numeric(lv_at(r,state,i*2-1))==member)return i;
  return 0;
}
bool dg_button_hilite(dg_runtime_t *d,uint32_t member) {
  const dg_member_t *m=dg_member(d,member);if(!m || m->type!=7)return false;
  unsigned slot=button_slot(d,member);
  lv_t value=slot?lv_at(d->values,d->values->roots[DG_BUTTON_STATE_ROOT],slot*2):(lv_t){0};
  return lv_type(value)==LV_VOID?(dg_button_kind(m)&&m->filled!=0):lv_truth(d->values,value);
}
void dg_button_set_hilite(dg_runtime_t *d,uint32_t member,bool value) {
  const dg_member_t *m=dg_member(d,member);if(!m || m->type!=7)return;
  if(dg_button_hilite(d,member)==value)return;
  lv_runtime_t *r=d->values;unsigned slot=button_slot(d,member);
  if(lv_type(r->roots[DG_BUTTON_STATE_ROOT])==LV_VOID)
    r->roots[DG_BUTTON_STATE_ROOT]=lv_list(r,0,NULL,true);
  lv_t state=r->roots[DG_BUTTON_STATE_ROOT];
  if(!slot){slot=lv_count(r,state)+1;lv_set_at(r,state,slot*2-1,lv_num(member));}
  lv_set_at(r,state,slot*2,lv_num(value));d->visual_revision++;
}
#if DG_EXTENDED
static void button_forget(dg_runtime_t *d,uint32_t member) {
  unsigned slot=button_slot(d,member);
  if(slot)lv_set_at(d->values,d->values->roots[DG_BUTTON_STATE_ROOT],slot*2,(lv_t){0});
}
#endif
static void button_press(dg_runtime_t *d,unsigned channel) {
  const dg_member_t *m=channel<DG_SPRITES?dg_member(d,d->sprites[channel].value.member):NULL;
  if(dg_button_kind(m)==DG_BUTTON_PUSH)dg_button_set_hilite(d,m->id,true);
}
static void button_release(dg_runtime_t *d,unsigned channel,bool inside) {
  const dg_member_t *m=channel<DG_SPRITES?dg_member(d,d->sprites[channel].value.member):NULL;
  unsigned kind=dg_button_kind(m);if(!kind)return;
  if(kind==DG_BUTTON_PUSH)dg_button_set_hilite(d,m->id,false);
  else if(inside)dg_button_set_hilite(d,m->id,kind==DG_BUTTON_RADIO || !dg_button_hilite(d,m->id));
}
