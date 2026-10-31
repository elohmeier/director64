// Included by the runtime and its source-free hit-test contract.
// Keep stage and input capabilities authoritative for every adapter, including
// older custom state printers. Observation does not advance the game clock.
static void state(void) {
  raw_state();
  while (output_length && isspace((unsigned char)output[output_length - 1]))
    output[--output_length] = 0;
  if (!output_length || output[output_length - 1] != '}') return;
  output[--output_length] = 0;
  emit(",\"stage\":[%u,%u],\"capabilities\":{\"keyboard\":%s,\"text\":%s}}\n",
       WC_WIDTH, WC_HEIGHT, DG_CAP_KEYBOARD ? "true" : "false",
       DG_EXTENDED ? "true" : "false");
}
static void point(unsigned sprite) {
  int l, t, r, b;
  dg_bounds(&director, sprite, &l, &t, &r, &b);
  int cx = (l + r) / 2, cy = (t + b) / 2;
  if (cx >= 8 && cx <= WC_WIDTH - 10 && cy >= 8 && cy <= WC_HEIGHT - 10 &&
      dg_mouse_hit(&director, cx, cy) == sprite) {
    emit("{\"point\":[%d,%d],\"error\":\"\"}\n", cx, cy);
    return;
  }
  for (int spacing = 8; spacing >= 1; spacing /= 8)
    for (int yy = t; yy < b; yy += spacing)
      for (int xx = l; xx < r; xx += spacing)
        if (xx >= 8 && xx <= WC_WIDTH - 10 && yy >= 8 && yy <= WC_HEIGHT - 10 &&
            dg_mouse_hit(&director, xx, yy) == sprite) {
          emit("{\"point\":[%d,%d],\"error\":\"\"}\n", xx, yy);
          return;
        }
  emit("{\"point\":null,\"error\":\"\"}\n");
}
