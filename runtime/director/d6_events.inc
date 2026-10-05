// Outgoing behavior lists live on a native continuation frame until endSprite
// and stopMovie finish. Nothing unloads their code or applies a new score early.
static lv_flow_t d6_cleanup_step(lv_runtime_t *r, lv_frame_t *f) {
  dg_runtime_t *d = r->context;
  lv_t pairs = f->locals[0];
  unsigned count = lv_count(r, pairs);
  while (f->pc < count) {
    unsigned channel = (unsigned)lv_integer(r, lv_at(r, pairs, f->pc + 1));
    lv_t list = lv_at(r, pairs, f->pc + 2);
    unsigned index = (unsigned)lv_integer(r, f->locals[5]);
    if (index < lv_count(r, list)) {
      f->locals[5] = lv_num(index + 1);
      d->current_event_sprite = channel;
      if (lv_start_method(r, lv_at(r, list, index + 1), "endsprite", 0, NULL)) return LV_CALL;
    } else {
      f->pc += 2;
      f->locals[5] = lv_num(0);
    }
  }
  d->ending_sprites = false;
  bool movie = lv_truth(r, f->locals[4]);
  if (f->pc == count) {
    f->pc++;
    d->current_event_sprite = 0;
    if (movie && dg_event(d, "stopmovie", 0)) return LV_CALL;
  }
  d->current_event_sprite = (unsigned)lv_integer(r, f->locals[6]);
  if (movie) {
    d->movie_ready = true;
    if (d->quit_requested) d->quit = true;
  } else if (lv_truth(r, f->locals[2])) {
    advance_frame(d, (unsigned)lv_integer(r, f->locals[1]), lv_truth(r, f->locals[3]));
  } else {
    seek_now(d, (unsigned)lv_integer(r, f->locals[1]));
  }
  d->cleanup_active = false;
  r->result = f->locals[7];
  return LV_RETURN;
}
static const lv_handler_t d6_cleanup_handler = {
  .name="cleanup", .kind="MovieScript", .arguments=8, .locals=8, .step=d6_cleanup_step
};
static const lv_movie_t d6_cleanup_movie = {"<D6 cleanup>", 1, &d6_cleanup_handler, NULL, NULL, NULL, NULL, 0};
static bool d6_queue_cleanup(dg_runtime_t *d, unsigned frame, bool advance,
                              bool timed, bool movie) {
  if (d->cleanup_active || !d->frame || (!movie && frame == d->frame)) return false;
  const dg_behavior_t *target[DG_SPRITES] = {0};
  if (!movie) {
    if (!frame || frame > d->movie->frame_count) return false;
    if (frame > d->frame)
      for (unsigned i=1;i<DG_SPRITES;i++) target[i]=d->score[i+5].behaviors;
    for (unsigned f=frame>d->frame?d->frame+1:1;f<=frame;f++) {
      const dg_frame_t *source=&d->movie->frames[f-1];
      for (unsigned n=0;n<source->count;n++) {
        const dg_delta_t *delta=&d->movie->deltas[source->first+n];
        if (delta->channel>=6 && delta->channel<DG_SCORE_CHANNELS)
          target[delta->channel-5]=delta->value.behaviors;
      }
    }
  }
  lv_runtime_t *r=d->values;
  lv_t pairs={0}; unsigned entries=0;
  for (unsigned i=1;i<DG_SPRITES;i++) {
    lv_t list=r->roots[DG_BEHAVIOR_ROOT+i];
    if ((!movie && d->sprites[i].value.behaviors==target[i]) || lv_type(list)!=LV_LIST) continue;
    bool ending=false;
    for (unsigned n=1;n<=lv_count(r,list) && !ending;n++) {
      lv_t script=lv_get(r,NULL,"script",lv_at(r,list,n));
      const lv_movie_t *owner=NULL;
      ending=resolve_script(r,script,"endsprite",&owner)!=NULL;
    }
    if (!ending) continue;
    if (lv_type(pairs)==LV_VOID) pairs=lv_list(r,0,NULL,false);
    lv_set_at(r,pairs,++entries,lv_num(i));
    // Copy the attachment list so a callback cannot discard later receivers.
    unsigned size=lv_count(r,list);
    lv_t snapshot=lv_list(r,0,NULL,false);
    for (unsigned n=1;n<=size;n++) lv_set_at(r,snapshot,n,lv_at(r,list,n));
    lv_set_at(r,pairs,++entries,snapshot);
  }
  if (!entries && !movie) return false;
  if (lv_type(pairs)==LV_VOID) pairs=lv_list(r,0,NULL,false);
  lv_t args[]={pairs,lv_num(frame),lv_num(advance),lv_num(timed),lv_num(movie),
               lv_num(0),lv_num(d->current_event_sprite),r->result};
  d->cleanup_active=d->ending_sprites=true;
  lv_start(r,&d6_cleanup_movie,"cleanup",0,8,args);
  return true;
}
