// Director's Flash Normal/Fixed/Lock-Step controls, independent instance
// clocks, and the stream-audio bridge. Rate values use 16.16 frames/second.
static unsigned flash_setting(dg_runtime_t *d, const dg_member_t *m,
                              bool create) {
  unsigned n = 0;
  while (n < d->flash_playback_count && d->flash_playback[n].member != m->id)
    n++;
  if (n == d->flash_playback_count && create) {
    if (n == DG_FIELDS) {
      error(d, "Flash playback settings capacity", m->name ? m->name : "");
      return DG_FIELDS;
    }
    d->flash_playback[n].member = m->id;
    d->flash_playback[n].mode = m->flash_playback_mode;
    d->flash_playback[n].fixed_rate = m->flash_fixed_rate;
    d->flash_playback[n].paused = !!(m->video_flags & 256);
    d->flash_playback_count++;
  }
  return n;
}
static bool flash_initial_paused(dg_runtime_t *d, const dg_member_t *m) {
  unsigned n = flash_setting(d, m, false);
  return n < d->flash_playback_count ? d->flash_playback[n].paused
                                     : !!(m->video_flags & 256);
}
static uint32_t flash_rate(dg_runtime_t *d, const dg_member_t *m,
                           const dg_sprite_t *s, unsigned *mode_out) {
  unsigned mode = m->flash_playback_mode;
  uint32_t fixed = m->flash_fixed_rate;
  unsigned n = flash_setting(d, m, false);
  if (n < d->flash_playback_count) {
    mode = d->flash_playback[n].mode;
    fixed = d->flash_playback[n].fixed_rate;
  }
  if (s) {
    if (s->flash_settings & 1)
      mode = s->flash_mode;
    if (s->flash_settings & 2)
      fixed = s->flash_fixed_rate;
  }
  if (mode_out)
    *mode_out = mode;
  // Source-free legacy models without intrinsic-rate metadata keep their
  // established lock-step behavior. Converted Flash always supplies a rate.
  return mode == 2 || !m->flash_rate ? 0 : mode == 1 ? fixed : m->flash_rate;
}
static bool flash_playback_property(dg_runtime_t *d, const char *name,
                                    lv_t owner, lv_t *value, bool write) {
  unsigned ln = lv_name_id(d->values, name);
  if (ln != LN_PLAYBACKMODE && ln != LN_FIXEDRATE && ln != LN_PAUSEDATSTART && ln != LN_FRAMERATE)
    return false;
  dg_sprite_t *s = lv_type(owner) == LV_SPRITE ? sprite(d, owner) : NULL;
  const dg_member_t *m = s ? dg_member(d, s->value.member)
                         : lv_type(owner) == LV_MEMBER
                             ? dg_member(d, (uint32_t)lv_id(owner))
                             : NULL;
  if (!m || m->source_xtra != 1)
    return false;
  if(ln==LN_FRAMERATE) {
    if(write)error(d,"Flash frameRate is read-only",name);
    else *value=lv_num((double)m->flash_rate/65536);
    return true;
  }
  unsigned mode = 0, n = flash_setting(d, m, false);
  (void)flash_rate(d, m, s, &mode);
  uint32_t fixed = n < d->flash_playback_count ? d->flash_playback[n].fixed_rate
                                               : m->flash_fixed_rate;
  bool paused = flash_initial_paused(d, m);
  if (s && (s->flash_settings & 2))
    fixed = s->flash_fixed_rate;
  if (!write) {
    *value = ln == LN_PAUSEDATSTART ? lv_num(paused)
             : ln == LN_PLAYBACKMODE
                 ? lv_symbol(d->values, mode == 2   ? "lockStep"
                                        : mode == 1 ? "fixed"
                                                    : "normal")
                 : lv_num((double)fixed / 65536);
    return true;
  }
  if (ln == LN_PLAYBACKMODE) {
    const char *text = lv_cstr(d->values, *value);
    if (eq(text, "normal"))
      mode = 0;
    else if (eq(text, "fixed"))
      mode = 1;
    else if (eq(text, "lockstep"))
      mode = 2;
    else {
      error(d, "invalid Flash playback mode", text);
      return true;
    }
  } else if (ln == LN_FIXEDRATE) {
    double number = lv_number(d->values, *value);
    if (!isfinite(number) || number <= 0 || number > 999) {
      error(d, "invalid Flash fixed rate", name);
      return true;
    }
    fixed = (uint32_t)llround(number * 65536);
  } else
    paused = lv_truth(d->values, *value);
  if (ln == LN_PAUSEDATSTART && s) {
    error(d, "Flash pausedAtStart requires member", name);
    return true;
  }
  if (s) {
    if (ln == LN_PLAYBACKMODE) {
      s->flash_mode = (uint8_t)mode;
      s->flash_settings |= 1;
    } else {
      s->flash_fixed_rate = fixed;
      s->flash_settings |= 2;
    }
    s->flash_phase = dg_is_flash_audio(m) ? dg_film_pose(m, s) : 0;
  } else {
    n = flash_setting(d, m, true);
    if (n == DG_FIELDS)
      return true;
    d->flash_playback[n].mode = (uint8_t)mode;
    d->flash_playback[n].fixed_rate = fixed;
    d->flash_playback[n].paused = paused;
    for (unsigned i = 1; i < DG_SPRITES; i++)
      if (d->sprites[i].value.member == m->id)
        d->sprites[i].flash_phase =
            dg_is_flash_audio(m) ? dg_film_pose(m, &d->sprites[i]) : 0;
  }
  return true;
}
static bool flash_audio_position(const dg_member_t *m, unsigned frame,
                                 double *position) {
  unsigned previous = frame;
  while (previous && m->flash_audio_positions[previous] == UINT32_MAX)
    previous--;
  uint32_t base = m->flash_audio_positions[previous];
  if (base == UINT32_MAX)
    return false;
  // A missing block does not stop a preceding block's audio. Seeking into
  // such a frame advances from its preceding authored sample anchor.
  *position = fmin(m->samples, base + (double)(frame - previous) * m->rate *
                                          65536 / m->flash_rate);
  return true;
}
static void flash_audio_sync(dg_runtime_t *d, dg_sprite_t *s,
                             const dg_member_t *m) {
  if (!dg_is_flash_audio(m))
    return;
  unsigned frame = dg_film_pose(m, s), mode;
  uint32_t rate = flash_rate(d, m, s, &mode);
  double position = 0;
  bool available = flash_audio_position(m, frame, &position);
  if (s->media_frame != frame) {
    double time = (double)position * 600 / m->rate;
    double tolerance = m->flash_rate ? 600.0 * 65536 / m->flash_rate : 10;
    if (available &&
        (s->media_frame == UINT32_MAX || frame != s->media_frame + 1 ||
         fabs(time - s->video_time) > tolerance)) {
      s->video_time = time;
      s->video_serial++;
    }
    s->media_frame = frame;
  }
  double fps = rate ? (double)rate / 65536
                    : (d->puppet_tempo ? d->puppet_tempo : d->tempo);
  s->video_rate =
      s->film_paused || !available ? 0 : fps * 65536 / m->flash_rate;
}
static void flash_tick(dg_runtime_t *d, dg_sprite_t *s, const dg_member_t *m) {
  uint32_t rate = flash_rate(d, m, s, NULL);
  bool stream = dg_is_flash_audio(m);
  if (rate && (!stream || !s->film_paused))
    s->flash_phase += (double)rate / (65536 * 60);
  if (!stream) {
    if (s->flash_phase > 2)
      s->flash_phase = fmod(s->flash_phase, 1) + 1;
    return;
  }
  flash_audio_sync(d, s, m);
  if (!s->film_paused && s->video_rate) {
    s->video_time += 10 * s->video_rate;
    if (s->video_time > dg_media_duration(m))
      s->video_time = dg_media_duration(m);
  }
}
static void flash_seek(dg_runtime_t *d, dg_sprite_t *s, const dg_member_t *m) {
  if (!m || m->source_xtra != 1)
    return;
  if(m->flash_clip_count)
    dg_flash_seek(d,(unsigned)(s-d->sprites),dg_film_pose(m,s),!s->film_paused);
  s->flash_phase = dg_is_flash_audio(m) ? dg_film_pose(m, s) : 0;
  if (dg_is_flash_audio(m)) {
    s->media_frame = UINT32_MAX;
    flash_audio_sync(d, s, m);
  }
}
// Called at a Director frame opportunity. Streaming sound is the time
// master and may skip displayed poses; modeled Stop actions still run.
static bool flash_advance(dg_runtime_t *d, dg_sprite_t *s,
                          const dg_member_t *m) {
  uint32_t rate = flash_rate(d, m, s, NULL);
  if (rate && !dg_is_flash_audio(m)) {
    if (s->flash_phase + 1e-9 < 1)
      return false;
    s->flash_phase = fmod(s->flash_phase, 1);
  }
  if (m->flash_variant_count > 1) {
    s->flash_clock = (uint8_t)((s->flash_clock + 1u) % m->flash_variant_count);
    dg_sprite_changed(d, (unsigned)(s - d->sprites));
  }
  unsigned old = dg_film_pose(m, s), next = old + 1;
  if (rate && dg_is_flash_audio(m))
    next = (unsigned)(s->flash_phase + 1e-9);
  if(m->flash_clip_count) {
    bool changed=dg_flash_advance(d,(unsigned)(s-d->sprites),next);
    if(dg_is_flash_audio(m)) {
      if(s->film_paused)s->flash_phase=dg_film_pose(m,s);
      else if(next>=m->film_count)s->flash_phase=fmod(s->flash_phase,m->film_count);
    }
    flash_audio_sync(d,s,m);
    return changed;
  }
  if (s->film_paused)
    return false;
  bool looping = member_looping(d, m);
  unsigned steps = next >= old ? next - old : 0;
  unsigned nearest = UINT32_MAX;
  for (unsigned i = 0; i < m->flash_stop_count; i++) {
    unsigned stop = m->flash_stops[i] - 1u;
    unsigned distance = stop > old ? stop - old
                        : looping  ? m->film_count - old + stop
                                   : UINT32_MAX;
    if (distance && distance <= steps && distance < nearest) {
      nearest = distance;
      next = stop;
      s->film_paused = true;
    }
  }
  bool wrapped = !s->film_paused && next >= m->film_count;
  if (!looping && next >= m->film_count)
    next = m->film_count - 1u;
  else if (looping)
    next %= m->film_count;
  s->film_frame = next;
  if (wrapped && looping && dg_is_flash_audio(m)) {
    s->flash_phase = fmod(s->flash_phase, m->film_count);
    s->media_frame = UINT32_MAX;
  }
  if (s->film_paused)
    flash_seek(d, s, m);
  flash_audio_sync(d, s, m);
  return next != old;
}
