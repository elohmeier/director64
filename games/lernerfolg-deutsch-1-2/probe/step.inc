// Lernerfolg's probe step for the web runtime's probe
// (platforms/web/web_runtime.c), which includes it after defining values, director, enter(), find() and the output helpers.
static bool step(int x, int y, unsigned buttons) {
  bool down = (buttons & 1u) != 0; // input.h's INPUT_A, the left button
  // Focused-window boot: a pending controller-window movie enters the
  // single context; its code stays resident through dg_window_loaded.
  if (director.window_movie_name[0] && !director.window_movie &&
      !director.next_movie[0] && !values.depth)
    snprintf(director.next_movie, sizeof(director.next_movie), "%.27s.DXR", director.window_movie_name);
  if (dg_transition_ready(&director)) {
    if (!enter(find(director.next_movie), director.next_movie_frame ? director.next_movie_frame : 1)) return false;
    dg_window_loaded(&director);
  }
  bool ok = dg_tick(&director, x, y, down, DG_SERVICE_BUDGET);
  dg_update_stage(&director);
  return ok && !values.failed;
}
