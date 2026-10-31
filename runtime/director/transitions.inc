// Numeric transition identifiers and millisecond cast fields are recovered
// format facts; see source transition records and clean ScummVM41ac2b3.
static bool queue_transition(dg_runtime_t *d, unsigned type, unsigned milliseconds,
                             unsigned chunk, unsigned area) {
  if (type > 52 || area > 1 || chunk > 65535 || milliseconds > 65535) {
    error(d, "transition parameters out of range", "puppetTransition");
    return false;
  }
  if(type==47 || type==48) {
    error(d,"transition geometry is not implemented", "puppetTransition");
    return false;
  }
  d->transition_type = type;
  d->transition_duration = (milliseconds * 60u + 999u) / 1000u;
  if (d->transition_duration < 15) d->transition_duration = 15;
  d->transition_chunk = chunk ? chunk : 1;
  d->transition_area = area;
  d->transition_serial++;
  d->transition_pending = type != 0;
  return true;
}
static bool puppet_transition(dg_runtime_t *d, unsigned argc, const lv_t *args) {
  lv_runtime_t *r = d->values;
  if (argc < 1 || argc > 4) {
    error(d, "transition argument count", "puppetTransition");
    return true;
  }
  if (lv_type(args[0]) == LV_MEMBER) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(args[0]));
    if (!m || m->type != 14 || !m->transition_present)
      error(d, "missing converted transition cast metadata", "puppetTransition");
    else
      queue_transition(d, m->transition_type, m->transition_ms,
                        m->transition_chunk, m->transition_area);
    return true;
  }
  int type = lv_integer(r, args[0]);
  int time = argc > 1 ? lv_integer(r, args[1]) : 1;
  int chunk = argc > 2 ? lv_integer(r, args[2]) : 1;
  int area = argc > 3 ? lv_integer(r, args[3]) : 1;
  if (type < 0 || time < 0 || time > 262 || chunk < 0 || area < 0)
    error(d, "transition parameters out of range", "puppetTransition");
  else
    queue_transition(d, (unsigned)type, (unsigned)time * 250u,
                      (unsigned)chunk, (unsigned)area);
  return true;
}
static bool begin_transition(dg_runtime_t *d) {
  if (!d->transition_pending || !d->transition_type) return false;
  d->transition_pending = false;
  d->transition_active = true;
  d->transition_start = d->ticks;
  d->frame_deadline += (uint64_t)d->transition_duration * 1000000u;
  d->frame_stalled = true;
  return true;
}
static bool transition_waiting(dg_runtime_t *d) {
  if (!d->transition_active) return false;
  if (d->ticks - d->transition_start < d->transition_duration && !d->mouse_pressed)
    return true;
  d->transition_active = false;
  if (d->mouse_pressed) {
    // A click skips a reveal; it is consumed by the presentation itself.
    unsigned elapsed = d->ticks - d->transition_start;
    if (elapsed < d->transition_duration)
      d->frame_deadline -= (uint64_t)(d->transition_duration - elapsed) * 1000000u;
    d->mouse_pressed = false;
    d->mouse_down = d->mouse_released = d->mouse_up_dispatch = false;
#if DG_EXTENDED
    d->mouse_down_dispatch = false;
#endif
  }
  return false;
}
