/* Shared probe cost accounting and heap census, included by each game's
 * tests/director_probe.c after its `values` and `director` definitions.
 *
 * perf_account/perf_report: per-movie service cost in CPU microseconds. The
 * console budget is one 60 Hz tick; this host proxy locates over-budget
 * scenes and regressions without an emulator pass. Each report covers the
 * work since the previous one, so a caller polling during a walk
 * accumulates every tick exactly once.
 *
 * heap_report: live value-heap census after a collection — object handles,
 * bytes by value type and the largest single objects. Handle pressure, not
 * just byte use, drives collector cost: the interval between collections is
 * the number of FREE handles, so a session whose live set grows collects
 * hyperbolically more often.
 */
static struct { char movie[32]; uint64_t ticks, tick_us, stage_us, max_us; } perf_rows[96];
static unsigned perf_rows_used;
static uint64_t cpu_us(void) { return (uint64_t)clock() * 1000000 / CLOCKS_PER_SEC; }
static void perf_account(const char *movie, uint64_t tick_us, uint64_t stage_us) {
  unsigned i = 0;
  while (i < perf_rows_used && strcmp(perf_rows[i].movie, movie)) i++;
  if (i == perf_rows_used) {
    if (i == sizeof(perf_rows) / sizeof(*perf_rows)) return;
    perf_rows_used++;
    snprintf(perf_rows[i].movie, sizeof(perf_rows[i].movie), "%s", movie);
  }
  perf_rows[i].ticks++; perf_rows[i].tick_us += tick_us; perf_rows[i].stage_us += stage_us;
  if (tick_us + stage_us > perf_rows[i].max_us) perf_rows[i].max_us = tick_us + stage_us;
}
static void perf_report(void) {
  printf("{\"perf\":[");
  for (unsigned i = 0; i < perf_rows_used; i++) {
    if (i) putchar(',');
    printf("{\"movie\":\"%s\",\"ticks\":%llu,\"tick_us\":%llu,\"stage_us\":%llu,\"max_us\":%llu}",
           perf_rows[i].movie, (unsigned long long)perf_rows[i].ticks,
           (unsigned long long)perf_rows[i].tick_us,
           (unsigned long long)perf_rows[i].stage_us,
           (unsigned long long)perf_rows[i].max_us);
    perf_rows[i].ticks = perf_rows[i].tick_us = perf_rows[i].stage_us = 0;
    perf_rows[i].max_us = 0;
  }
  puts("]}"); fflush(stdout);
}
/* Indexed by lv_type_t so a census label can never drift from the enum. */
static const char *const heap_type_names[] = {
  [LV_VOID] = "void", [LV_NUMBER] = "number", [LV_STRING] = "string",
  [LV_SYMBOL] = "symbol", [LV_BYTES] = "bytes", [LV_LIST] = "list",
  [LV_PROPLIST] = "proplist", [LV_SPRITE] = "sprite", [LV_MEMBER] = "member",
  [LV_FIELD] = "field", [LV_FILE] = "file", [LV_SCRIPT] = "script",
  [LV_INSTANCE] = "instance", [LV_XTRA] = "xtra", [LV_SOUND] = "sound",
  [LV_COLOR] = "color", [LV_CASTLIB] = "castlib", [LV_WINDOW] = "window",
#if DIRECTOR64_DIRECTOR_VERSION >= 10
  [LV_GLOBALS_VIEW] = "globals", [LV_IMAGE] = "image", [LV_TIMEOUT] = "timeout",
#endif
};
static void heap_report(void) {
  lv_collect(&values);
  enum { HEAP_KINDS = sizeof(heap_type_names) / sizeof(*heap_type_names) };
  unsigned counts[HEAP_KINDS] = {0}, bytes[HEAP_KINDS] = {0}, handles = 0;
  unsigned top[8] = {0};
  for (unsigned i = 0; i < LV_OBJECTS; i++) {
    lv_object_t *o = &values.objects[i];
    if (!o->type) continue;
    handles++;
    unsigned size = o->count * (o->type >= LV_LIST ? (unsigned)sizeof(lv_t) : 1u);
    unsigned kind = o->type < HEAP_KINDS ? o->type : 0;
    counts[kind]++; bytes[kind] += size;
    unsigned candidate = i, candidate_size = size;
    for (unsigned t = 0; t < 8; t++) {
      lv_object_t *held = &values.objects[top[t]];
      unsigned other = top[t] ? held->count * (held->type >= LV_LIST ? (unsigned)sizeof(lv_t) : 1u) : 0;
      if (candidate_size > other) {
        unsigned swap = top[t]; top[t] = candidate; candidate = swap; candidate_size = other;
      }
    }
  }
  printf("{\"heap\":{\"used\":%u,\"high_water\":%u,\"limit\":%u,"
         "\"object_handles\":%u,\"object_limit\":%u,\"collections\":%lu,"
         "\"emergency_collections\":%lu,\"types\":{",
         values.heap_used, values.heap_high_water, (unsigned)LV_HEAP_BYTES,
         handles, (unsigned)LV_OBJECTS, (unsigned long)values.collect_passes,
         (unsigned long)values.emergency_passes);
  bool comma = false;
  for (unsigned k = 0; k < HEAP_KINDS; k++) {
    if (!counts[k]) continue;
    if (comma) putchar(',');
    comma = true;
    printf("\"%s\":{\"objects\":%u,\"bytes\":%u}",
           heap_type_names[k] ? heap_type_names[k] : "?", counts[k], bytes[k]);
  }
  printf("},\"largest\":[");
  for (unsigned t = 0; t < 8; t++) {
    if (!top[t]) continue;
    lv_object_t *o = &values.objects[top[t]];
    unsigned size = o->count * (o->type >= LV_LIST ? (unsigned)sizeof(lv_t) : 1u);
    if (t) putchar(',');
    printf("{\"type\":\"%s\",\"count\":%u,\"bytes\":%u}",
           o->type < HEAP_KINDS && heap_type_names[o->type] ? heap_type_names[o->type] : "?",
           o->count, size);
  }
  puts("]}}"); fflush(stdout);
}
