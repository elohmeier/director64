// Löwenzahn's probe step, shared by the native probe (tests/director_probe.c)
// and the browser runtime (platforms/web/web_runtime.c), which both include it
// after defining values, director, pointer, enter(), find() and the output helpers.
static bool step(int x, int y, unsigned buttons) {
  input_sample_t sample = {.connected=true, .buttons=buttons};
  if (game_print_input(&sample)) return true;
  if (director.next_movie[0] && !values.depth && !enter(find(director.next_movie), director.next_movie_frame ? director.next_movie_frame : 1)) return false;
  bool ok = dg_tick(&director, x, y, !!(buttons & INPUT_A), DG_SERVICE_BUDGET);
  dg_update_stage(&director);draw_record_tick(&director);
  // The N64 renderer reads changed field values as text, without coercion.
  for (unsigned i = 0; i < director.field_count; i++)
    (void)lv_cstr(&values, values.roots[DG_FIELD_TEXT_ROOT + i]);
  return ok && !values.failed;
}
