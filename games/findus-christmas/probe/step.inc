// Christmas's probe step for the web runtime's probe
// (platforms/web/web_runtime.c), which includes it after defining values, director, enter(), find() and the output helpers.
// Buttons are input.h bits; the score reads the left button (INPUT_A, 1).
static bool step(int x, int y, unsigned buttons) {
  if (dg_transition_ready(&director) && !enter(find(director.next_movie), director.next_movie_frame ? director.next_movie_frame : 1)) return false;
  bool ok = dg_tick(&director, x, y, (buttons & 1u) != 0, DG_SERVICE_BUDGET);
  dg_update_stage(&director); return ok && !values.failed;
}
