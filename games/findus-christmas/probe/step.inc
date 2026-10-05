// Christmas's probe step, shared by the native probe (tests/director_probe.c)
// and the browser runtime (platforms/web/web_runtime.c), which both include it
// after defining values, director, enter(), find() and the output helpers.
// Buttons are input.h bits; the score reads the left button (INPUT_A, 1).
static bool step(int x, int y, unsigned buttons) {
  if (dg_transition_ready(&director) && !enter(find(director.next_movie), director.next_movie_frame ? director.next_movie_frame : 1)) return false;
  bool ok = dg_tick(&director, x, y, (buttons & 1u) != 0, DG_SERVICE_BUDGET);
  dg_update_stage(&director);draw_record_tick(&director); return ok && !values.failed;
}
