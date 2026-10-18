// Host player IDs include the linked movie instance, so equal sprite channel
// numbers in independent movies never share decoding or sound state.
typedef struct {
  unsigned id;
  uint32_t member;
} web_video_entry_t;
typedef struct {
  dg_runtime_t *owner;
  const dg_member_t *member;
  unsigned channel, id;
  bool sound, paused;
} web_video_request_t;
static web_video_entry_t video_entries[DG_SPRITES];

static void close_videos(void) {
  for (unsigned i = 0; i < DG_SPRITES; i++)
    if (video_entries[i].id && host.video)
      host.video(host.ctx, video_entries[i].id, NULL, "", 0, 0, 0, 0);
  memset(video_entries, 0, sizeof(video_entries));
}
static bool collect_videos(dg_runtime_t *d, web_video_request_t *requests,
                           unsigned *count, bool sound, bool paused) {
  for (unsigned channel = 1; channel < DG_SPRITES; channel++) {
    const dg_sprite_t *s = &d->sprites[channel];
    const dg_member_t *m = s->value.type ? dg_member(d, s->value.member) : NULL;
    if (dg_has_media(m)) {
      if (*count == DG_SPRITES) {
        lv_fail(d->values, "concurrent digital video capacity exceeded");
        return false;
      }
      unsigned id = dg_video_player_id(d, channel);
      requests[(*count)++] = (web_video_request_t){d, m, channel, id, sound, paused};
    }
#if DG_D10
    if (dg_linked_movie(d, channel)) {
      dg_runtime_t *child = dg_linked_begin(d, channel);
      bool ok = child && collect_videos(child, requests, count,
                                        sound && m && (m->linked_flags & 8),
                                        paused || s->film_paused || child->linked_paused);
      dg_linked_end(d);
      if (!ok)
        return false;
    }
#endif
  }
  return true;
}
static void update_videos(void) {
  if (!host.video)
    return;
  web_video_request_t requests[DG_SPRITES];
  unsigned count = 0;
  if (!collect_videos(&director, requests, &count, true, false))
    return;
  // Close departed owners before allocating their replacements. No stale
  // runtime pointer is dereferenced after a child has been disposed.
  for (unsigned i = 0; i < DG_SPRITES; i++) {
    web_video_entry_t *entry = &video_entries[i];
    if (!entry->id)
      continue;
    bool found = false;
    for (unsigned j = 0; j < count; j++)
      if (entry->id == requests[j].id &&
          entry->member == requests[j].member->id)
        found = true;
    if (!found) {
      host.video(host.ctx, entry->id, NULL, "", 0, 0, 0, 0);
      memset(entry, 0, sizeof(*entry));
    }
  }
  for (unsigned j = 0; j < count; j++) {
    const web_video_request_t *request = &requests[j];
    const dg_runtime_t *d = request->owner;
    const dg_sprite_t *s = &d->sprites[request->channel];
    const dg_member_t *m = request->member;
    unsigned slot = 0;
    while (slot < DG_SPRITES && video_entries[slot].id != request->id)
      slot++;
    if (slot == DG_SPRITES) {
      slot = 0;
      while (slot < DG_SPRITES && video_entries[slot].id)
        slot++;
      if (slot == DG_SPRITES) {
        lv_fail(director.values, "concurrent digital video capacity exceeded");
        return;
      }
      video_entries[slot] = (web_video_entry_t){request->id, m->id};
    }
    float volume = d->sound_level <= 0   ? 0.0f
                   : d->sound_level >= 7 ? 1.0f
                                         : d->sound_level / 7.0f;
    const char *video = dg_is_video(m) && m->asset && !(m->video_flags & 512) ? m->asset : "";
    const char *audio = request->sound && m->video_audio && dg_media_sound(m)
                            ? m->video_audio
                            : "";
    host.video(host.ctx, request->id, video, audio, s->video_time,
               request->paused ? 0 : s->video_rate, s->video_serial,
               volume * s->video_volume / 255.0f);
  }
}
