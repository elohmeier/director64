// Local SWA cast playback. Source audio is decoded at conversion, so preload
// completes synchronously. State values follow Macromedia's SWA API: stopped0,
// ready2, playing3, paused4, finished5 and file error9. Missing retail files
// retain state9 and a diagnostic; they never turn into manufactured silence.
static int swa_slot(dg_runtime_t *d, const dg_member_t *m, bool create) {
  if (!m || m->source_xtra != 6 || m->type != 6) return -1;
  unsigned i = 0;
  for (; i < d->swa_count; i++) if (d->swa[i].member == m->id) return (int)i;
  if (!create) return -1;
  if (i == DG_FIELDS) { error(d, "SWA member capacity", m->name); return -1; }
  d->swa_count++;
  d->swa[i].member = m->id;
  d->swa[i].volume = 255;
  d->swa[i].channel = 4; // SWA's independent channel, after the four score channels.
  return (int)i;
}
static unsigned swa_state(dg_runtime_t *d, unsigned i) {
  unsigned ch = d->swa[i].channel;
  if (d->swa[i].state == 3 &&
      (d->sounds[ch] != d->swa[i].member || !d->platform.sound_busy ||
       !d->platform.sound_busy(d->context, ch))) d->swa[i].state = 5;
  return d->swa[i].state;
}
static bool swa_get(dg_runtime_t *d, const char *name, lv_t owner, lv_t *out) {
  if (lv_type(owner) != LV_MEMBER) return false;
  const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
  if (!m || m->source_xtra != 6) return false;
  lv_runtime_t *r = d->values;
  int i = swa_slot(d, m, true); if (i < 0) return true;
  bool available = m->asset && *m->asset && m->rate && m->samples;
  if (eq(name, "state")) *out = lv_num(swa_state(d, (unsigned)i));
  else if (eq(name, "percentstreamed")) *out = lv_num(available ? 100 : 0);
  else if (eq(name, "duration")) *out = lv_num(m->rate ? (double)m->samples * 1000 / m->rate : 0);
  else if (eq(name, "volume")) *out = lv_num(d->swa[i].volume);
  else if (eq(name, "soundchannel")) *out = lv_num(d->swa[i].channel + 1);
  else if (eq(name, "samplerate")) *out = lv_num(m->rate);
  else if (eq(name, "url") || eq(name, "streamname") || eq(name, "filename"))
    *out = lv_text(r, m->text ? m->text : "", false);
  else return false;
  return true;
}
static bool swa_set(dg_runtime_t *d, const char *name, lv_t owner, lv_t value) {
  if (lv_type(owner) != LV_MEMBER) return false;
  const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
  if (!m || m->source_xtra != 6) return false;
  int i = swa_slot(d, m, true); if (i < 0) return true;
  if (eq(name, "volume")) {
    unsigned v = (unsigned)fmax(0, fmin(255, lv_number(d->values, value)));
    d->swa[i].volume = (uint16_t)v;
    unsigned ch = d->swa[i].channel;
    if (d->sounds[ch] == m->id) d->channel_volume[ch] = (uint16_t)v;
    return true;
  }
  return false;
}
static bool swa_call(dg_runtime_t *d, const char *name, unsigned argc, const lv_t *args, lv_t *out) {
  if (argc != 1 || lv_type(args[0]) != LV_MEMBER) return false;
  const dg_member_t *m = dg_member(d, (uint32_t)lv_id(args[0]));
  if (!m || m->source_xtra != 6 ||
      !(eq(name, "play_frame") || eq(name, "play") || eq(name, "stop") || eq(name, "preloadbuffer"))) return false;
  int i = swa_slot(d, m, true); if (i < 0) return true;
  unsigned ch = d->swa[i].channel;
  if (eq(name, "stop")) {
    if (d->sounds[ch] == m->id) play_member(d, ch, NULL);
    if (d->sounds[ch] == m->id) d->sounds[ch] = 0;
    d->swa[i].state = 0;
  } else if (!m->asset || !*m->asset || !m->rate || !m->samples) {
    d->swa[i].state = 9;
    if (d->platform.trace) {
      char message[160];
      snprintf(message, sizeof(message), "SWA_FILE_ERROR %s %s", m->name, m->text ? m->text : "");
      d->platform.trace(d->context, message);
    }
  } else if (eq(name, "preloadbuffer")) d->swa[i].state = 2;
  else {
    d->sound_puppet[ch] = true;
    d->sounds[ch] = m->id;
    d->sound_serial[ch]++;
    d->channel_volume[ch] = d->swa[i].volume;
    play_member(d, ch, m);
    d->swa[i].state = 3;
  }
  *out = lv_num(0);
  return true;
}
