/* Retained SWF dictionary metadata. Counts and references are checked before
 * executable actions can reach the VM. */
static bool read_flash_actions(loader_t *l, const dg_flash_action_t **out,
                               uint16_t *count_out, unsigned frames) {
  reader_t *r=l->reader; unsigned count=u16(r);
  if (!need(r,(size_t)count*12)) return false;
  dg_flash_action_t *actions=count ? allocate(l,count,sizeof(*actions)) : NULL;
  *out=actions; *count_out=(uint16_t)count;
  for (unsigned i=0;actions && i<count;i++) {
    dg_flash_action_t *a=&actions[i];
    a->frame=u16(r);a->event=u32(r);a->key=u8(r);a->length=u32(r);
    if (a->frame>frames || !a->length || a->length>1048576 || !need(r,a->length))
      return fail(r,"invalid Flash action metadata");
    uint8_t *code=allocate(l,a->length,1); if (!code) return false;
    memcpy(code,r->data+r->at,a->length);r->at+=a->length;a->code=code;
    unsigned p=0;
    while (p<a->length) {
      uint8_t op=code[p++];
      if (!op) {if (p!=a->length) return fail(r,"bytes after Flash ActionEnd");break;}
      if (op>=128) {
        if (a->length-p<2) return fail(r,"truncated Flash action length");
        unsigned n=code[p]|code[p+1]<<8;p+=2;
        if (n>a->length-p) return fail(r,"truncated Flash action bytes");
        p+=n;
      }
    }
    if (code[a->length-1]) return fail(r,"Flash action has no terminator");
  }
  return !r->failed;
}
static bool read_flash_frame(loader_t *l,dg_film_frame_t *frame,unsigned movies) {
  reader_t *r=l->reader;frame->count=u16(r);
  if (frame->count>=1000 || !need(r,(size_t)frame->count*108)) return fail(r,"Flash layer count exceeds profile");
  dg_film_layer_t *layers=frame->count ? allocate(l,frame->count,sizeof(*layers)) : NULL;
  frame->layers=layers;
  for (unsigned i=0;layers && i<frame->count;i++) {
    if (!read_film_layer(l,&layers[i],movies)) return false;
    if (i && layers[i-1].channel>=layers[i].channel) return fail(r,"unordered Flash layers");
  }
  return !r->failed;
}
static bool flash_bounds(reader_t *r,int32_t *bounds) {
  for (unsigned i=0;i<4;i++) bounds[i]=(int32_t)u32(r);
  return bounds[2]>=0 && bounds[3]>=0 ? !r->failed : fail(r,"negative Flash bounds");
}
static bool flash_frame_refs(reader_t *r,const dg_member_t *m,const dg_film_frame_t *f) {
  for (unsigned i=0;i<f->count;i++) {
    const dg_film_layer_t *v=&f->layers[i];bool c=!v->clip_id,b=!v->button_id,t=!v->field_id;
    if (v->asset && !v->mask_asset) return fail(r,"retained Flash raster has no geometric mask asset");
    for (unsigned n=0;!c && n<m->flash_clip_count;n++) c=m->flash_clips[n].id==v->clip_id;
    for (unsigned n=0;!b && n<m->flash_button_count;n++) b=m->flash_buttons[n].id==v->button_id;
    for (unsigned n=0;!t && n<m->flash_field_count;n++) t=m->flash_fields[n].id==v->field_id;
    if (!c || !b || !t) return fail(r,"Flash layer references absent definition");
  }
  return true;
}
static bool read_flash_runtime(loader_t *l,dg_member_t *m,unsigned movies) {
  reader_t *r=l->reader;
  m->flash_clip_count=u16(r);m->flash_button_count=u16(r);m->flash_sound_count=u16(r);m->flash_export_count=u16(r);
  if (m->flash_clip_count>4096 || m->flash_button_count>4096 || m->flash_sound_count>4096 || m->flash_export_count>4096)
    return fail(r,"Flash dictionary exceeds profile");
  dg_flash_clip_t *clips=m->flash_clip_count ? allocate(l,m->flash_clip_count,sizeof(*clips)) : NULL;m->flash_clips=clips;
  dg_flash_button_t *buttons=m->flash_button_count ? allocate(l,m->flash_button_count,sizeof(*buttons)) : NULL;m->flash_buttons=buttons;
  dg_flash_sound_t *sounds=m->flash_sound_count ? allocate(l,m->flash_sound_count,sizeof(*sounds)) : NULL;m->flash_sounds=sounds;
  dg_flash_export_t *exports=m->flash_export_count ? allocate(l,m->flash_export_count,sizeof(*exports)) : NULL;m->flash_exports=exports;
  bool root=false;uint8_t identities[8192]={0};
#define FLASH_ID(id) do { unsigned n=(id); if (identities[n>>3]&(1u<<(n&7))) return fail(r,"duplicate Flash definition identity"); identities[n>>3]|=(uint8_t)(1u<<(n&7)); } while(0)
  for (unsigned i=0;i<m->flash_field_count;i++) if (m->flash_fields[i].id) {FLASH_ID(m->flash_fields[i].id);}
  for (unsigned i=0;clips && i<m->flash_clip_count;i++) {
    dg_flash_clip_t *c=&clips[i];c->id=u16(r);FLASH_ID(c->id);c->frame_count=u16(r);
    if (!c->frame_count || c->frame_count>4096 || (!c->id && c->frame_count!=m->film_count) || !flash_bounds(r,c->local_bounds))
      return fail(r,"invalid Flash clip metadata");
    if (!c->id) root=true;
    unsigned count=c->id ? c->frame_count : 0;
    dg_film_frame_t *frames=count ? allocate(l,count,sizeof(*frames)) : NULL;c->frames=frames;
    for (unsigned f=0;frames && f<count;f++) if (!read_flash_frame(l,&frames[f],movies)) return false;
    if (!read_flash_actions(l,&c->actions,&c->action_count,c->frame_count)) return false;
    c->label_count=u16(r);if (!need(r,(size_t)c->label_count*6)) return false;
    dg_label_t *labels=c->label_count ? allocate(l,c->label_count,sizeof(*labels)) : NULL;c->labels=labels;
    for (unsigned n=0;labels && n<c->label_count;n++) {labels[n].name=string(l);labels[n].frame=u16(r);
      if (!labels[n].frame || labels[n].frame>c->frame_count) return fail(r,"Flash label outside clip");}
    c->sound_event_count=u16(r);if (!need(r,(size_t)c->sound_event_count*16)) return false;
    dg_flash_sound_event_t *events=c->sound_event_count ? allocate(l,c->sound_event_count,sizeof(*events)) : NULL;c->sound_events=events;
    for (unsigned n=0;events && n<c->sound_event_count;n++) {
      dg_flash_sound_event_t *e=&events[n];e->frame=u16(r);e->sound=u16(r);e->loops=u16(r);e->flags=u8(r);e->envelope_count=u8(r);
      e->in_point=u32(r);e->out_point=u32(r);
      if (!e->frame || e->frame>c->frame_count || (e->flags&0xc0) || !need(r,(size_t)e->envelope_count*8)) return fail(r,"invalid Flash sound event");
      dg_flash_envelope_t *env=e->envelope_count ? allocate(l,e->envelope_count,sizeof(*env)) : NULL;e->envelopes=env;
      for (unsigned k=0;env && k<e->envelope_count;k++) {env[k].position=u32(r);env[k].left=u16(r);env[k].right=u16(r);
        if (env[k].left>32768 || env[k].right>32768 || (k && env[k-1].position>=env[k].position))
          return fail(r,"invalid Flash sound envelope");}
    }
  }
  if (m->flash_clip_count && (!root || !m->film_frames)) return fail(r,"Flash runtime has no root frames");
  for (unsigned i=0;buttons && i<m->flash_button_count;i++) {
    dg_flash_button_t *b=&buttons[i];b->id=u16(r);FLASH_ID(b->id);uint8_t menu=u8(r);b->track_as_menu=menu!=0;
    if (!b->id || menu>1 || !flash_bounds(r,b->local_bounds)) return fail(r,"invalid Flash button");
    for (unsigned n=0;n<4;n++) if (!read_flash_frame(l,&b->states[n],movies)) return false;
    if (!read_flash_actions(l,&b->actions,&b->action_count,0)) return false;
  }
  for (unsigned i=0;sounds && i<m->flash_sound_count;i++) {
    dg_flash_sound_t *s=&sounds[i];s->id=u16(r);FLASH_ID(s->id);s->asset=string(l);s->samples=u32(r);s->rate=u32(r);
    if (!s->id || !s->asset || !*s->asset || !s->samples || !s->rate) return fail(r,"invalid Flash event sound");
  }
  for (unsigned i=0;exports && i<m->flash_export_count;i++) {
    exports[i].id=u16(r);exports[i].name=string(l);unsigned n=exports[i].id;
    if (!(identities[n>>3]&(1u<<(n&7))) || !exports[i].name || !*exports[i].name) return fail(r,"invalid Flash export");
  }
  m->flash_initializer_count=u16(r);
  if (m->flash_initializer_count>4096) return fail(r,"too many Flash initializers");
  dg_flash_initializer_t *initializers=m->flash_initializer_count ? allocate(l,m->flash_initializer_count,sizeof(*initializers)) : NULL;
  m->flash_initializers=initializers;
  for (unsigned i=0;initializers && i<m->flash_initializer_count;i++) {
    dg_flash_initializer_t *init=&initializers[i];init->clip_id=u16(r);
    const dg_flash_action_t *actions=NULL;uint16_t count=0;
    if (!read_flash_actions(l,&actions,&count,m->film_count)) return false;
    bool found=false;for (unsigned k=0;k<m->flash_clip_count;k++) if (clips[k].id==init->clip_id) found=true;
    if (!init->clip_id || !found || count!=1 || !actions[0].frame || actions[0].event || actions[0].key)
      return fail(r,"invalid Flash initializer");
    init->action=actions[0];
  }

#undef FLASH_ID
  for (unsigned i=0;clips && i<m->flash_clip_count;i++) {
    const dg_flash_clip_t *c=&clips[i];const dg_film_frame_t *frames=c->id ? c->frames : m->film_frames;
    for (unsigned n=0;n<c->frame_count;n++) if (!flash_frame_refs(r,m,&frames[n])) return false;
    for (unsigned n=0;n<c->sound_event_count;n++) {bool found=false;
      for (unsigned k=0;k<m->flash_sound_count;k++) if (sounds[k].id==c->sound_events[n].sound) found=true;
      if (!found) return fail(r,"Flash event references absent sound");}
  }
  for (unsigned i=0;buttons && i<m->flash_button_count;i++) for (unsigned n=0;n<4;n++) if (!flash_frame_refs(r,m,&buttons[i].states[n])) return false;
  return !r->failed;
}
