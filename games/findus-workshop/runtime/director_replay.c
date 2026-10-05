// Linked only into full-probe; the playable full ROM has no replay code.
#include "director_replay.h"
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
typedef struct {
  unsigned ticks;
  int x, y;
  unsigned down; // Mouse 0/1, or raw button bits for FULL_REPLAY_CONTROLLER.
} replay_step_t;
#include "replay.inc"
static unsigned index_, elapsed;
static bool complete;
#if defined(FULL_REPLAY_ADAPTIVE_MOSSEN) ||                                    \
    defined(FULL_REPLAY_ADAPTIVE_PLOCK) || defined(FULL_REPLAY_ADAPTIVE_MAP) || \
    defined(FULL_REPLAY_ADAPTIVE_VEMORY) || defined(FULL_REPLAY_ADAPTIVE_CANS)
static unsigned control_phase, control_tick;
#ifdef FULL_REPLAY_ADAPTIVE_MOSSEN
static const char *const control_names[] = {
    "speletslut",  "gAntalGuldFeather", "timecounter", "laktivobj",
    "ghoppsprite", "clhotspotx",        "clhotspoty"};
#elif defined(FULL_REPLAY_ADAPTIVE_PLOCK)
static const char *const control_names[] = {"difficulty", "gAntalGuldFeather",
                                            "objaktiv", "ypos", "xpos"};
#elif defined(FULL_REPLAY_ADAPTIVE_VEMORY)
static const char *const control_names[] = {
    "gmode", "gantalguldfeather", "lslumppos", "antalverktygframme",
    "verkpos1", "clminx", "clminy", "clmaxx", "clmaxy",
    "clmode2obj1", "clmode2obj2", "clmode3obj1", "clmode3obj2"};
#elif defined(FULL_REPLAY_ADAPTIVE_CANS)
static const char *const control_names[] = {
    "bilfardklar", "foregaenderattsvarat", "gantalguldfeather", "gantalburkar",
    "gvald", "gsumma", "gantalposter", "clburkxpos", "clburkypos", "clwidth", "clheight"};
#else
static const char *const control_names[] = {"lskattkartamatris",
                                            "lskattmatris"};
#endif
static int control_ids[sizeof(control_names) / sizeof(*control_names)];
static lv_t control_value(dg_runtime_t *d, unsigned n) {
  return d->values->globals[control_ids[n]];
}
#ifndef FULL_REPLAY_ADAPTIVE_MAP
static int control_number(dg_runtime_t *d, unsigned n) {
  return lv_integer(d->values, control_value(d, n));
}
#endif
#ifdef FULL_REPLAY_ADAPTIVE_MOSSEN
static void feedback_mouse(input_sample_t *sample, dg_runtime_t *d) {
  int x = 620, y = 20;
  bool down = false;
  unsigned age = d->ticks - control_tick;
  if (control_phase == 0 && control_number(d, 0) == 1) {
    if (control_number(d, 1) != 0) {
      lv_fail(d->values, "feedback: failed round awarded a feather");
      return;
    }
    debugf("DIRECTOR64 FULL_CONTROL_ASSERT initial_failure\n");
    control_phase = 1;
    control_tick = d->ticks;
    age = 0;
  }
  if (control_phase == 1) {
    x = 610;
    y = 430;
    // The end-of-round speech still owns the source event loop when its
    // game-over flag first appears. Retry through normal clicks until it ends.
    down = age >= 600 && age % 180 < 30;
    if (age >= 20 && !control_number(d, 0) && control_number(d, 2) < 10) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT retry\n");
      control_phase = 2;
      control_tick = d->ticks;
    }
  } else if (control_phase == 2) {
    if (control_number(d, 1) == 1) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT reward\n");
      control_phase = 3;
      control_tick = d->ticks;
    } else {
      lv_t active = control_value(d, 3);
      for (unsigned i = 1; i <= lv_count(d->values, active); i++) {
        int value = lv_integer(d->values, lv_at(d->values, active, i));
        if ((value == 2 || value == 3) && (int)i != control_number(d, 4)) {
          x = lv_integer(d->values, lv_at(d->values, control_value(d, 5), i));
          y = lv_integer(d->values, lv_at(d->values, control_value(d, 6), i));
          break;
        }
      }
    }
  } else if (control_phase == 3 && age >= 2000) {
    control_phase = 4;
    control_tick = d->ticks;
  } else if (control_phase == 4) {
    x = y = 30;
    down = age < 20;
    if (!strcmp(d->movie->code->name, "BYRAN.DXR")) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT return\n");
      control_phase = 5;
    }
  }
  sample->pointer_x = x * INPUT_ONE;
  sample->pointer_y = y * INPUT_ONE;
  sample->buttons = down ? INPUT_A : 0;
}
#elif defined(FULL_REPLAY_ADAPTIVE_PLOCK)
static void feedback_mouse(input_sample_t *sample, dg_runtime_t *d) {
  int x = 320, y = 380;
  bool down = false;
  unsigned age = d->ticks - control_tick;
  if (control_phase == 0 && d->ticks >= 1200) {
    if (control_number(d, 0) == FULL_REPLAY_ADAPTIVE_PLOCK) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT difficulty\n");
      control_phase = 1;
    } else {
      int left, top, right, bottom;
      dg_bounds(d, 6, &left, &top, &right, &bottom);
      x = (left + right) / 2;
      y = (top + bottom) / 2;
      down = (d->ticks - 1200) % 360 < 20;
    }
  } else if (control_phase == 1) {
    if (control_number(d, 1) == 1) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT reward\n");
      control_phase = 2;
      control_tick = d->ticks;
    } else {
      int highest = -32768;
      for (unsigned i = 1; i <= 6; i++) {
        int active =
            lv_integer(d->values, lv_at(d->values, control_value(d, 2), i));
        int height =
            lv_integer(d->values, lv_at(d->values, control_value(d, 3), i));
        if (active == 2 && height < 405 && height > highest) {
          highest = height;
          x = lv_integer(d->values, lv_at(d->values, control_value(d, 4), i));
        }
      }
    }
  } else if (control_phase == 2 && age >= 2500) {
    if (!strcmp(d->movie->code->name, "MVEGGEN.DXR")) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT return\n");
      control_phase = 3;
    } else {
      for (unsigned i = 1; i < DG_SPRITES; i++) {
        if ((d->sprites[i].value.member & 65535) == 147) {
          int left, top, right, bottom;
          dg_bounds(d, i, &left, &top, &right, &bottom);
          x = (left + right) / 2;
          y = (top + bottom) / 2;
          down = (age - 2500) % 360 < 20;
          break;
        }
      }
    }
  }
  sample->pointer_x = x * INPUT_ONE;
  sample->pointer_y = y * INPUT_ONE;
  sample->buttons = down ? INPUT_A : 0;
}
#elif defined(FULL_REPLAY_ADAPTIVE_CANS)
static int control_x = 320, control_y = 240;
static bool control_point, initial_failure;
static int can_value(dg_runtime_t *d, unsigned list, unsigned index) {
  return lv_integer(d->values, lv_at(d->values, control_value(d, list), index));
}
static bool can_sprite(dg_runtime_t *d, unsigned sprite) {
  int left, top, right, bottom;
  dg_bounds(d, sprite, &left, &top, &right, &bottom);
  for (int y = top; y < bottom; y += 2)
    for (int x = left; x < right; x += 2)
      if (x >= 8 && x < 630 && y >= 8 && y < 470 && dg_mouse_hit(d, x, y) == sprite) {
        control_x = x; control_y = y; return true;
      }
  return false;
}
static void feedback_mouse(input_sample_t *sample, dg_runtime_t *d) {
  bool down = false;
  unsigned age = d->ticks - control_tick;
  if (control_phase == 0 && d->frame == 3 && !d->values->depth) {
    control_phase = 1; control_tick = d->ticks;
  } else if (control_phase == 1) {
    if (control_number(d, 0)) {
      control_phase = 2; control_tick = d->ticks;
    } else {
      unsigned round = age % 180;
      if (!round) {
        int value = 1, count = 1;
        if (initial_failure) {
          int total = control_number(d, 5);
          for (value = 1; value <= 10; value++)
            if (total > 0 && total % value == 0 && total / value <= 10) break;
          if (value > 10) { lv_fail(d->values, "feedback: invalid can total"); return; }
          count = total / value;
        }
        if (control_number(d, 3) == count && control_number(d, 4) == value) {
          control_point = can_sprite(d, 6);
        } else {
          control_x = can_value(d, 7, value) - can_value(d, 9, value) / 2;
          control_y = can_value(d, 8, value) - can_value(d, 10, value) / 2;
          control_point = true;
        }
      }
      down = control_point && round < 20;
    }
  } else if (control_phase == 2) {
    if (!initial_failure) {
      if (control_number(d, 1) || control_number(d, 2)) {
        lv_fail(d->values, "feedback: wrong can answer rewarded"); return;
      }
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT initial_failure\n");
      initial_failure = true;
    }
    if (control_number(d, 2) == 1) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT reward\n");
      control_phase = 4; control_tick = d->ticks;
    } else if (age >= 1800) {
      unsigned round = (age - 1800) % 180;
      if (!round) control_point = can_sprite(d, 46);
      down = control_point && round == 0;
      if (!control_number(d, 0) && !control_number(d, 3) && !d->values->depth) {
        control_phase = 3; control_tick = d->ticks;
      }
    }
  } else if (control_phase == 3 && age >= 300) {
    control_phase = 1; control_tick = d->ticks;
  } else if (control_phase == 4 && age >= 1800) {
    control_x = 30; control_y = 20;
    down = (age - 1800) % 180 < 20;
    if (!strcmp(d->movie->code->name, "BYRAN.DXR")) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT return\n");
      control_phase = 5; down = false;
    }
  }
  sample->pointer_x = control_x * INPUT_ONE;
  sample->pointer_y = control_y * INPUT_ONE;
  sample->buttons = down ? INPUT_A : 0;
}
#elif defined(FULL_REPLAY_ADAPTIVE_VEMORY)
static int control_x = 320, control_y = 240;
static bool control_point;
static int board_value(dg_runtime_t *d, unsigned list, unsigned index) {
  return lv_integer(d->values, lv_at(d->values, control_value(d, list), index));
}
static bool board_point(dg_runtime_t *d, unsigned index) {
  int left = board_value(d, 5, index), top = board_value(d, 6, index);
  int right = board_value(d, 7, index), bottom = board_value(d, 8, index);
  for (int y = top + 1; y < bottom; y++) {
    for (int x = left + 1; x < right; x++) {
      unsigned hit = 0;
      for (unsigned i = 1; i <= 30; i++) {
        if (board_value(d, 2, i) && x >= board_value(d, 5, i) &&
            x <= board_value(d, 7, i) && y >= board_value(d, 6, i) &&
            y <= board_value(d, 8, i)) { hit = i; break; }
      }
      if (hit == index) { control_x = x; control_y = y; return true; }
    }
  }
  return false;
}
static void feedback_mouse(input_sample_t *sample, dg_runtime_t *d) {
  bool down = false;
  unsigned age = d->ticks - control_tick;
  if (control_phase == 0) {
    if (d->frame == 19) {
      int left, top, right, bottom;
      const unsigned buttons[] = {0,42,44,43};
      dg_bounds(d, buttons[FULL_REPLAY_ADAPTIVE_VEMORY], &left, &top, &right, &bottom);
      control_x = (left + right) / 2; control_y = (top + bottom) / 2;
      down = d->ticks % 120 < 20;
    } else if (d->frame == 3 && control_number(d, 0) == FULL_REPLAY_ADAPTIVE_VEMORY &&
               lv_count(d->values, control_value(d, 2)) == 30) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT difficulty\n");
      control_phase = 1;
      control_tick = d->ticks;
    }
  } else if (control_phase == 1) {
    if (control_number(d, 1) == 1) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT reward\n");
      control_phase = 2; control_tick = d->ticks;
    } else {
      unsigned round = age % 180;
      if (!round) {
        unsigned index = 0;
        int wanted = 0;
        if (control_number(d, 3) == 1) {
          int selected = board_value(d, 2, (unsigned)control_number(d, 4));
          wanted = -selected;
          if (FULL_REPLAY_ADAPTIVE_VEMORY > 1) {
            unsigned first = FULL_REPLAY_ADAPTIVE_VEMORY == 2 ? 9 : 11;
            unsigned from = selected > 0 ? first : first + 1;
            unsigned to = selected > 0 ? first + 1 : first;
            for (unsigned i = 1; i <= lv_count(d->values, control_value(d, from)); i++) {
              if (board_value(d, from, i) == abs(selected)) {
                wanted = board_value(d, to, i) * (selected > 0 ? -1 : 1);
                break;
              }
            }
          }
        }
        for (unsigned i = 1; i <= 30; i++) {
          int value = board_value(d, 2, i);
          if (wanted ? value == wanted : value > 0) { index = i; break; }
        }
        control_point = index && board_point(d, index);
      }
      down = control_point && round < 20;
    }
  } else if (control_phase == 2 && age >= 3000) {
    control_x = 40; control_y = 35;
    down = (age - 3000) % 180 < 20;
    if (!strcmp(d->movie->code->name, "PINTRO.DXR")) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT return\n");
      control_phase = 3; down = false;
    }
  }
  sample->pointer_x = control_x * INPUT_ONE;
  sample->pointer_y = control_y * INPUT_ONE;
  sample->buttons = down ? INPUT_A : 0;
}
#else
static int control_x = 620, control_y = 20;
static bool control_point;
static bool map_point(dg_runtime_t *d, unsigned sprite) {
  int left, top, right, bottom;
  dg_bounds(d, sprite, &left, &top, &right, &bottom);
  control_x = (left + right) / 2;
  control_y = (top + bottom) / 2;
  if (dg_mouse_hit(d, control_x, control_y) == sprite)
    return true;
  for (int y = top; y < bottom; y += 2)
    for (int x = left; x < right; x += 2)
      if (x >= 8 && x < 630 && y >= 8 && y < 470 &&
          dg_mouse_hit(d, x, y) == sprite) {
        control_x = x;
        control_y = y;
        return true;
      }
  return false;
}
static void feedback_mouse(input_sample_t *sample, dg_runtime_t *d) {
  int x = 620, y = 20;
  bool down = false;
  unsigned age = d->ticks - control_tick;
  if (control_phase == 0 && d->ticks >= 1800) {
    unsigned missing = 0;
    for (unsigned i = 1; i <= 9; i++)
      if (!lv_integer(d->values, lv_at(d->values, control_value(d, 0), i))) {
        missing = i;
        break;
      }
    if (!missing) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT all_nine_fragments\n");
      control_phase = 1;
      control_tick = d->ticks;
    } else {
      unsigned round = (d->ticks - 1800) % 360;
      if (!round)
        control_point = map_point(d, 30 + missing);
      if (control_point) {
        x = control_x;
        y = control_y;
        down = round < 20;
      }
    }
  } else if (control_phase == 1) {
    lv_t treasures = control_value(d, 1);
    bool found = false;
    for (unsigned i = 1; i <= lv_count(d->values, treasures); i++) {
      int treasure = lv_integer(d->values, lv_at(d->values, treasures, i));
      found |= treasure == 34 || treasure == 35;
    }
    if (found) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT special_treasure\n");
      control_phase = 2;
      control_tick = d->ticks;
    } else if (age >= 1200) {
      x = 448;
      y = 326;
      down = (age - 1200) % 1320 < 120;
    }
  } else if (control_phase == 2 && age >= 1800) {
    x = 40;
    y = 35;
    down = (age - 1800) % 360 < 20;
    if (!strcmp(d->movie->code->name, "PINTRO.DXR")) {
      debugf("DIRECTOR64 FULL_CONTROL_ASSERT return\n");
      control_phase = 3;
      down = false;
    }
  }
  sample->pointer_x = x * INPUT_ONE;
  sample->pointer_y = y * INPUT_ONE;
  sample->buttons = down ? INPUT_A : 0;
}
#endif
#endif
static void global(lv_runtime_t *r, const char *name, lv_t value) {
  int id = lv_global_id(r, name);
  if (id >= 0)
    r->globals[id] = value;
}
const char *director_replay_init(lv_runtime_t *r) {
  r->random_state = 42;
#if defined(FULL_REPLAY_ADAPTIVE_MOSSEN) ||                                    \
    defined(FULL_REPLAY_ADAPTIVE_PLOCK) || defined(FULL_REPLAY_ADAPTIVE_MAP) || \
    defined(FULL_REPLAY_ADAPTIVE_VEMORY) || defined(FULL_REPLAY_ADAPTIVE_CANS)
  for (unsigned i = 0; i < sizeof(control_names) / sizeof(*control_names);
       i++) {
    control_ids[i] = lv_global_id(r, control_names[i]);
    if (control_ids[i] < 0) {
      lv_fail(r, "feedback controller global missing");
      return replay_initial_movie;
    }
  }
#endif
  if (strcmp(replay_initial_movie, "START.DXR")) {
    global(r, "gpathdatafiles", lv_text(r, "C:\\data\\", false));
    global(r, "gvaktfilename", lv_text(r, "vakt1.txt", false));
    global(r, "lskattmatris",
           lv_literal(r,
                      "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
                      "0,0,0,0,0,0,0,0]"));
    global(r, "lskattkartamatris", lv_literal(r, "[0,0,0,0,0,0,0,0,0]"));
  }
  debugf("DIRECTOR64 FULL_REPLAY_START id=%s source=%s\n", replay_id,
         replay_source_sha256);
  return replay_initial_movie;
}
void director_replay_sample(input_sample_t *sample, dg_runtime_t *d) {
#ifdef FULL_REPLAY_CONTROLLER
  *sample = (input_sample_t){.connected = true};
#else
  *sample = (input_sample_t){.connected = true,
                             .pointer_absolute = true,
                             .pointer_x = 320 * INPUT_ONE,
                             .pointer_y = 240 * INPUT_ONE};
#endif
  if (index_ >= sizeof(replay_steps) / sizeof(*replay_steps)) {
    if (!complete) {
      int id = lv_global_id(d->values, "gAntalGuldFeather");
      debugf("DIRECTOR64 FULL_REPLAY_COMPLETE id=%s movie=%s frame=%u ticks=%lu "
             "feathers=%ld\n",
             replay_id, d->movie->code->name, d->frame, (unsigned long)d->ticks,
             id >= 0 ? (long)lv_integer(d->values, d->values->globals[id])
                     : 0L);
      complete = true;
    }
    return;
  }
  const replay_step_t *step = &replay_steps[index_];
#ifdef FULL_REPLAY_CONTROLLER
  sample->stick_x = step->x;
  sample->stick_y = step->y;
  sample->buttons = step->down;
#else
  sample->pointer_x = step->x * INPUT_ONE;
  sample->pointer_y = step->y * INPUT_ONE;
  sample->buttons = step->down ? INPUT_A : 0;
#endif
#if defined(FULL_REPLAY_ADAPTIVE_MOSSEN) ||                                    \
    defined(FULL_REPLAY_ADAPTIVE_PLOCK) || defined(FULL_REPLAY_ADAPTIVE_MAP) || \
    defined(FULL_REPLAY_ADAPTIVE_VEMORY) || defined(FULL_REPLAY_ADAPTIVE_CANS)
  feedback_mouse(sample, d);
#endif
  if (++elapsed == step->ticks) {
    elapsed = 0;
    index_++;
  }
}
