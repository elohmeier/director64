// Director 5 services used by Löwenzahn. Score records remain versioned;
// Lingo's default video time scale is 60 ticks per second. Converted media and
// the target decoder use 600 units per second internally; convert at the API.
static unsigned d5_number(dg_runtime_t *d, uint32_t id) {
  unsigned cast = member_cast(d, id);
  return (cast > 1 ? (cast - 1) << 17 : 0) | (id & 65535);
}
static void d5_media_init(dg_runtime_t *d, dg_sprite_t *s) {
  if (s->media_member == s->value.member)
    return;
  const dg_member_t *m = dg_member(d, s->value.member);
  s->media_member = s->value.member;
  s->media_frame = UINT32_MAX;
  s->video_time = 0;
  s->video_start = 0;
  s->video_stop = 0;
  s->video_volume = 255;
  s->video_serial++;
  s->video_rate = m && m->type == 10 && !(m->video_flags & 256) ? 1 : 0;
}
static bool d5_get(dg_runtime_t *d, const char *name, lv_t owner, lv_t *out) {
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
  if (lv_type(owner) == LV_VOID) {
    if (ln == LN_ROLLOVER) {
      *out = lv_num(dg_mouse_hit(d, d->mouse_x, d->mouse_y));
      return true;
    }
    if (ln == LN_COMMANDDOWN) {
      *out = lv_num(0);
      return true;
    }
    if (ln == LN_ACTORLIST) {
      *out = r->roots[DG_BEHAVIOR_ROOT + DG_SPRITES];
      return true;
    }
    if (ln == LN_QUICKTIMEPRESENT) {
      *out = lv_num(1);
      return true;
    }
    if (ln == LN_STILLDOWN) {
      *out = lv_num(d->mouse_down);
      return true;
    }
    if (ln == LN_DIGITALVIDEOTIMESCALE) {
      *out = lv_num(60);
      return true;
    }
    if (ln == LN_MOUSEDOWNSCRIPT) {
      *out = lv_text(r, d->mouse_down_script, false);
      return true;
    }
  } else if (lv_type(owner) == LV_SPRITE) {
    dg_sprite_t *s = sprite(d, owner);
    if (!s)
      return true;
    d5_media_init(d, s);
    if ((ln == LN_CASTNUM) || (ln == LN_MEMBERNUM)) {
      *out = lv_num(d5_number(d, s->value.member));
      return true;
    }
    if (ln == LN_MOVIETIME) {
      *out = lv_num((int)ceil(s->video_time / 10));
      return true;
    }
    if (ln == LN_MOVIERATE) {
      *out = lv_num(s->video_rate);
      return true;
    }
    if (ln == LN_STARTTIME) {
      *out = lv_num(s->video_start / 10);
      return true;
    }
    if (ln == LN_STOPTIME) {
      *out = lv_num(s->video_stop / 10);
      return true;
    }
    if (ln == LN_VOLUME) {
      *out = lv_num(s->video_volume);
      return true;
    }
  } else if (lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_FIELD) {
    const dg_member_t *m = dg_member(d, (uint32_t)lv_id(owner));
    if (ln == LN_NUMBER) {
      *out = lv_num(d5_number(d, (uint32_t)lv_id(owner)));
      return true;
    }
    if (m && m->type == 10) {
      if (ln == LN_DURATION) {
        *out = lv_num(ceil(m->samples / 10.0));
        return true;
      }
      if (ln == LN_TYPE) {
        *out = lv_text(r, "digitalVideo", true);
        return true;
      }
    }
  }
  return false;
}
static bool d5_set(dg_runtime_t *d, const char *name, lv_t owner, lv_t value) {
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
  if (lv_type(owner) == LV_WINDOW) {
    if ((ln == LN_RECT) || (ln == LN_FILENAME) || (ln == LN_WINDOWTYPE) ||
        (ln == LN_TITLEVISIBLE))
      return true;
  }
  if (lv_type(owner) == LV_VOID) {
    if (ln == LN_ACTORLIST) {
      r->roots[DG_BEHAVIOR_ROOT + DG_SPRITES] = value;
      return true;
    }
    if ((ln == LN_IDLELOADMODE) || (ln == LN_PRELOADEVENTABORT) ||
        (ln == LN_IDLELOADTAG))
      return true;
    if (ln == LN_MOUSEDOWNSCRIPT) {
      snprintf(d->mouse_down_script, sizeof(d->mouse_down_script), "%s",
               lv_cstr(r, value));
      return true;
    }
  } else if (lv_type(owner) == LV_SPRITE) {
    dg_sprite_t *s = sprite(d, owner);
    if (!s)
      return true;
    d5_media_init(d, s);
    if ((ln == LN_MOVIETIME) || (ln == LN_STARTTIME)) {
      s->video_time = fmax(0, lv_integer(r, value)) * 10;
      s->video_serial++;
      if (ln == LN_STARTTIME)
        s->video_start = (unsigned)s->video_time;
      return true;
    }
    if (ln == LN_STOPTIME) {
      s->video_stop = (unsigned)fmax(0, lv_integer(r, value)) * 10;
      return true;
    }
    if (ln == LN_MOVIERATE) {
      s->video_rate = lv_number(r, value);
      s->video_serial++;
      return true;
    }
    if (ln == LN_VOLUME) {
      s->video_volume = (unsigned)fmin(255, fmax(0, lv_number(r, value)));
      return true;
    }
  }
  return false;
}
static void d5_rollover_event(dg_runtime_t *d, const char *event,
                              unsigned channel) {
  if (!channel || channel >= DG_SPRITES)
    return;
  unsigned script = d->sprites[channel].value.script;
  if (((script && dg_event(d, event, script)) ||
       dg_event(d, event, d->sprites[channel].value.member)) &&
      d->values->depth) {
    lv_frame_t *f = &d->values->frames[d->values->depth - 1];
    if (f->handler->arguments)
      f->locals[0] = lv_num(channel);
  }
}
static const dg_member_t *d5_external_sound(dg_runtime_t *d, const char *filename) {
  // ScummVM util.cpp findAudioPath/resolvePath: exact name first, then replace
  // a three-letter extension (or append one) with .AIF and .WAV, in that order.
  // PANO's radio requests HOEli.dir, while the disc supplies HOELI.AIF.
  size_t length = strlen(filename), stem = length;
  if (length >= 4 && filename[length - 4] == '.' &&
      isalpha((unsigned char)filename[length - 3]) &&
      isalpha((unsigned char)filename[length - 2]) &&
      isalpha((unsigned char)filename[length - 1]))
    stem -= 4;
  const char *extensions[] = {NULL, ".aif", ".wav"};
  for (unsigned pass = 0; pass < 3; pass++) {
    const dg_member_t *found = NULL;
    for (unsigned f = 0; f < d->loaded_count; f++)
      for (unsigned i = 0; i < d->loaded[f]->member_count; i++) {
        const dg_member_t *m = &d->loaded[f]->members[i];
        if (m->type != 6)
          continue;
        if (!pass) {
          if (eq(m->name, filename))
            found = m;
          continue;
        }
        if (strlen(m->name) != stem + 4 || !eq(m->name + stem, extensions[pass]))
          continue;
        size_t n = 0;
        while (n < stem && tolower((unsigned char)m->name[n]) ==
                               tolower((unsigned char)filename[n]))
          n++;
        if (n == stem)
          found = m;
      }
    if (found)
      return found;
  }
  return NULL;
}
static bool d5_call(dg_runtime_t *d, const char *name, unsigned argc,
                    const lv_t *a, lv_t *out) {
  unsigned ln = lv_name_id(d->values, name);
  lv_runtime_t *r = d->values;
  if (((ln == LN_CONSTRAINH) || (ln == LN_CONSTRAINV)) && argc == 2) {
    // D2+ builtins clamp coordinates to the channel's live, inclusive bounds.
    int channel = lv_integer(r, a[0]), value = lv_integer(r, a[1]);
    *out = lv_num(0);
    if (channel > 0 && channel < DG_SPRITES) {
      int left, top, right, bottom;
      dg_bounds(d, (unsigned)channel, &left, &top, &right, &bottom);
      int low = (ln == LN_CONSTRAINH) ? left : top;
      int high = (ln == LN_CONSTRAINH) ? right : bottom;
      *out = lv_num(value < low ? low : value > high ? high : value);
    }
    return true;
  }
  if (((ln == LN_OPENXLIB) || (ln == LN_CLOSEXLIB)) && argc == 1 &&
      (eq(basename_(lv_cstr(r, a[0])), "pmatic.dll") ||
       eq(basename_(lv_cstr(r, a[0])), "pmatic.xobj")))
    return true;
  if (ln == LN_PRINTOMATIC) {
    // The source already branches on objectP and displays its no-printer alert.
    *out = (lv_t){0};
    return true;
  }
  if ((ln == LN_ALERT) && argc == 1) {
    const char *message = lv_cstr(r, a[0]);
    if (strstr(message, "Drucker"))
      message = "Drucken ist auf Nintendo 64 nicht verfuegbar.";
    snprintf(d->alert_text, sizeof(d->alert_text), "%s", message);
    d->alert_until = d->ticks + 240;
    return true;
  }
  if ((ln == LN_WINDOW) && argc == 1) {
    if (!eq(lv_cstr(r, a[0]), "dialog"))
      error(d, "unsupported movie window", lv_cstr(r, a[0]));
    *out = lv_make(LV_WINDOW, 1);
    return true;
  }
  if ((ln == LN_OPEN) && argc == 1 && lv_type(a[0]) == LV_WINDOW) {
    if (d->suspended_stage) {
      error(d, "nested movie window", "dialog");
      return true;
    }
    d->window_open = true;
    snprintf(d->next_movie, sizeof(d->next_movie), "DIALOG.DXR");
    d->next_movie_frame = 1;
    return true;
  }
  if (ln == LN_PAUSE) {
    d->source_paused = true;
    return true;
  }
  if ((ln == LN_FORGET) && argc == 1 &&
      (lv_type(a[0]) == LV_WINDOW || lv_type(a[0]) == LV_VOID))
    return true;
  if (ln == LN_CONTINUE) {
    if (d->suspended_stage) {
      dg_runtime_t *stage = d->suspended_stage;
      unsigned ticks = d->ticks;
      uint64_t clock = d->clock;
      lv_t roots = r->roots[LV_ROOTS - 1];
      // Sound-channel controls are shared with the suspended stage. Closing
      // the volume window must not restore its previous mixer settings.
      memcpy(stage->channel_volume, d->channel_volume, sizeof(d->channel_volume));
      memcpy(stage->fade_volume, d->fade_volume, sizeof(d->fade_volume));
      memcpy(stage->fade_start, d->fade_start, sizeof(d->fade_start));
      memcpy(stage->fade_duration, d->fade_duration, sizeof(d->fade_duration));
      *d = *stage;
      free(stage);
      d->suspended_stage = NULL;
      d->frame_deadline += clock - d->clock;
      d->clock = clock;
      d->ticks = ticks;
      for (unsigned i = 0; i < LV_ROOTS; i++)
        r->roots[i] = lv_at(r, roots, i + 1);
      r->current = d->movie->code;
      r->shared_count = d->loaded_count - 1;
      for (unsigned i = 1; i < d->loaded_count; i++)
        r->shared[i - 1] = d->loaded[i]->code;
      memset(r->call_cache, 0, sizeof(r->call_cache));
      d->window_cleanup = true;
      d->native_dialog = false;
      d->mouse_down_script[0] = d->mouse_up_script[0] = 0;
      d->input_event = 0;
      d->mouse_pressed = d->mouse_released = false;
      d->mouse_down = false;
      d->await_release = true;
      for (unsigned i = 1; i < DG_SPRITES; i++)
        dg_sprite_changed(d, i);
    }
    d->source_paused = false;
    return true;
  }
  if ((ln == LN_PASS) || (ln == LN_DONTPASSEVENT)) {
    d->pass_event = (ln == LN_PASS);
    return true;
  }
  if ((ln == LN_RTKREGISTERMAC) || (ln == LN_RTKREGISTERX16) ||
      (ln == LN_RTKREGISTERX32) || (ln == LN_PRELOADMOVIE) ||
      (ln == LN_PRELOAD))
    return true;
  if ((ln == LN_TELL_STAGE) || (ln == LN_TELL_END))
    return true;
  if ((ln == LN_CHECKFORROLLOVERS) || (ln == LN_ENDANYROLLOVERS)) {
    unsigned next = (ln == LN_ENDANYROLLOVERS)
                        ? 0
                        : dg_mouse_hit(d, d->mouse_x, d->mouse_y);
    unsigned old = d->rollover_sprite;
    if (next != old) {
      d->rollover_sprite = next;
      // Stack order preserves end-before-start when both receivers change.
      d5_rollover_event(d, "startrollover", next);
      d5_rollover_event(d, "endrollover", old);
    }
    return true;
  }
  if ((ln == LN_SOUND_PLAYFILE) && argc == 2) {
    int channel = lv_integer(r, a[0]);
    if (channel < 1 || channel > DG_SOUND_CHANNELS) {
      error(d, "sound channel", name);
      return true;
    }
    const char *filename = basename_(lv_cstr(r, a[1]));
    const dg_member_t *sound = d5_external_sound(d, filename);
    if (!sound) {
      // The original playFile of a missing file plays nothing and moves on.
      char text[96];
      snprintf(text, sizeof(text), "MISSING_SOUND %.64s", filename);
      if (d->platform.trace)
        d->platform.trace(d->context, text);
      return true;
    }
    d->sounds[channel - 1] = sound->id;
    d->sound_puppet[channel - 1] = true;
    d->sound_serial[channel - 1]++;
    play_member(d, (unsigned)channel - 1, sound);
    return true;
  }
  if ((ln == LN_PUPPETTRANSITION) && (argc == 1 || argc == 4)) {
    d->transition_type = (unsigned)lv_integer(r, a[0]);
    d->transition_serial++;
    return true;
  }
  if (((ln == LN_LABEL) || (ln == LN_MARKER)) && argc == 1) {
    unsigned frame = 0;
    if (lv_type(a[0]) == LV_STRING || lv_type(a[0]) == LV_SYMBOL) {
      for (unsigned i = 0; i < d->movie->label_count; i++)
        if (eq(d->movie->labels[i].name, lv_cstr(r, a[0])))
          frame = d->movie->labels[i].frame;
    } else if (d->movie->label_count) {
      int index = lv_integer(r, a[0]) - 1;
      if (ln == LN_MARKER) {
        int current = -1;
        for (unsigned i = 0; i < d->movie->label_count; i++)
          if (d->movie->labels[i].frame <= d->frame)
            current = (int)i;
        index = current + lv_integer(r, a[0]);
        if (current < 0 && !lv_integer(r, a[0])) {
          *out = lv_num(0);
          return true;
        }
      }
      if (index < 0)
        index = 0;
      if ((unsigned)index >= d->movie->label_count)
        index = (int)d->movie->label_count - 1;
      frame = d->movie->labels[index].frame;
    }
    *out = lv_num(frame);
    return true;
  }
  return false;
}
static bool d5_input(dg_runtime_t *d) {
  if (!d->input_event)
    return false;
  if (!d->pass_event) {
    d->input_event = 0;
    return false;
  }
  const char *event = d->input_event == 1 ? "mousedown" : "mouseup";
  dg_sprite_t *s = &d->sprites[d->input_channel];
  while (d->input_stage < 5) {
    unsigned stage = d->input_stage++;
    if (!stage) {
      const char *primary =
          d->input_event == 1 ? d->mouse_down_script : d->mouse_up_script;
      if (!*primary)
        continue;
      if (eq(primary, "dontpassevent")) {
        d->input_event = 0;
        return false;
      }
      char handler[80], argument[80];
      int n = sscanf(primary, "%79s %79s", handler, argument);
      if (n == 2 && eq(handler, "go")) {
        // OUTRO's click-to-skip primary: set the mouseDownScript to "go next".
        unsigned frame = frame_number(d, lv_text(d->values, argument, true));
        if (frame)
          d->next_frame = frame;
        continue;
      }
      int global = n == 2 ? lv_global_id(d->values, argument) : -1;
      if (global >= 0 && lv_start_method(d->values, d->values->globals[global],
                                         handler, 0, NULL))
        return true;
      if (n == 1 && dg_event(d, handler, 0))
        return true;
      script_error(d, "unresolved primary mouse handler", primary);
      return false;
    }
    unsigned member = stage == 1   ? s->value.script
                      : stage == 2 ? s->value.member
                      : stage == 3 ? d->score[0].script
                                   : 0;
    if (stage < 4 && !member)
      continue;
    if (dg_event(d, event, member)) {
      d->pass_event = false;
      return true;
    }
  }
  d->input_event = 0;
  return false;
}
static void d5_score_sounds(dg_runtime_t *d) {
  for (unsigned ch = 0; ch < 2; ch++) {
    if (d->sound_puppet[ch])
      continue;
    uint32_t id = d->score[ch ? 3 : 4].member;
    // A film's sound fills an empty score channel; higher sprites win.
    for (unsigned i = DG_SPRITES - 1; i && !id; i--) {
      const dg_sprite_t *s = &d->sprites[i];
      const dg_member_t *m = dg_member(d, s->value.member);
      if (s->value.type && m && m->type == 2 && m->film_count && m->film_sounds)
        id = m->film_sounds[dg_film_pose(m, s) * 2 + ch];
    }
    if (id == d->sounds[ch])
      continue;
    const dg_member_t *m = dg_member(d, id);
    if (id && (!m || m->type != 6)) {
      error(d, "score/film sound missing", d->movie->code->name);
      return;
    }
    d->sounds[ch] = id;
    d->sound_serial[ch]++;
    play_member(d, ch, m);
  }
}
static void d5_media_tick(dg_runtime_t *d) {
  for (unsigned i = 1; i < DG_SPRITES; i++) {
    dg_sprite_t *s = &d->sprites[i];
    const dg_member_t *m = dg_member(d, s->value.member);
    d5_media_init(d, s);
    if (!m || !s->value.type)
      continue;
    if (m->type == 10 && s->video_rate) {
      s->video_time += 10 * s->video_rate;
      unsigned stop = s->video_stop && s->video_stop < m->samples
                          ? s->video_stop
                          : m->samples;
      if (s->video_time >= stop) {
        if (m->video_flags & 16) {
          s->video_time = s->video_start;
          s->video_serial++;
        } else {
          s->video_time = stop;
          s->video_rate = 0;
          if (d->platform.trace) {
            char message[128];
            snprintf(message, sizeof(message),
                     "VIDEO_STOP member=%lu time=%u duration=%lu",
                     (unsigned long)m->id, stop, (unsigned long)m->samples);
            d->platform.trace(d->context, message);
          }
        }
      }
      dg_sprite_changed(d, i);
    }
  }
  d5_score_sounds(d);
}

static bool d5_waiting(dg_runtime_t *d) {
  unsigned wait = d->score_wait;
  if (!wait || d->puppet_tempo)
    return false;
  if (wait == 128)
    return true;
  if (wait == 134 || wait == 135)
    return d->platform.sound_busy &&
           d->platform.sound_busy(d->context, 135 - wait);
  if (wait >= 136 && wait < 135 + DG_SPRITES) {
    dg_sprite_t *s = &d->sprites[wait - 135];
    d5_media_init(d, s);
    const dg_member_t *m = dg_member(d, s->value.member);
    unsigned stop = m && s->video_stop && s->video_stop < m->samples
                        ? s->video_stop
                    : m ? m->samples
                        : 0;
    return m && m->type == 10 && s->video_time < stop;
  }
  return false;
}
