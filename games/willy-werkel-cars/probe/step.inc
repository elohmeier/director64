// Willy's probe step: its controller layer runs before the tick, shared by the native probe (tests/director_probe.c)
// and the browser runtime (platforms/web/web_runtime.c), which both include it
// after defining values, director, enter(), find() and the output helpers.
static bool step(int x, int y, unsigned buttons) {
  if (dg_transition_ready(&director) && !enter(find(director.next_movie), director.next_movie_frame ? director.next_movie_frame : 1)) return false;
  input_sample_t sample = {.connected=true,.buttons=(uint16_t)buttons};
  if (!willy_input(&director,&sample)) return true;
  bool ok = dg_tick(&director, x, y, (buttons&INPUT_A)!=0, DG_SERVICE_BUDGET);
  dg_update_stage(&director);draw_record_tick(&director); return ok && !values.failed && !values.script_error_count;
}
