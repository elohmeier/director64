// Mucklas's probe step for the web runtime's probe
// (platforms/web/web_runtime.c), which includes it after defining values, director, pointer, enter(), find() and the output helpers.
static bool step(int x, int y, unsigned buttons) {
  bool down = (buttons & 1u) != 0; // input.h's INPUT_A, the left button
  if (director.next_movie[0] && !values.depth && !enter(find(director.next_movie), director.next_movie_frame ? director.next_movie_frame : 1)) return false;
  bool ok = dg_tick(&director, x, y, down, DG_SERVICE_BUDGET);
  dg_update_stage(&director);
  return ok && !values.failed;
}
