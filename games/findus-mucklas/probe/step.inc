// Mucklas's probe step, shared by the native probe (tests/director_probe.c)
// and the browser runtime (platforms/web/web_runtime.c), which both include it
// after defining values, director, pointer, enter(), find() and the output helpers.
static bool step(int x, int y, unsigned buttons) {
  bool down = (buttons & 1u) != 0; // input.h's INPUT_A, the left button
  if (director.next_movie[0] && !values.depth && !enter(find(director.next_movie), director.next_movie_frame ? director.next_movie_frame : 1)) return false;
  const char *movie = director.movie ? director.movie->code->name : "";
  uint64_t started = cpu_us();
  bool ok = dg_tick(&director, x, y, down, DG_SERVICE_BUDGET);
  uint64_t serviced = cpu_us();
  dg_update_stage(&director);draw_record_tick(&director);
  perf_account(movie, serviced - started, cpu_us() - serviced);
  return ok && !values.failed;
}
