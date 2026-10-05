// The browser build of libdragon's mkfont (platforms/web/tools): mkfont
// packs each glyph atlas by running mksprite as a subprocess, which a
// WebAssembly module cannot. Force-included ahead of the pinned sources, this
// stands in for sheredom's subprocess.h (its include guard makes the real one
// a no-op): the "child" is a separate mksprite module the embedder runs on
// the atlas mkfont wrote to its stdin (Module.d64RunMksprite).
#ifndef SHEREDOM_SUBPROCESS_H_INCLUDED
#define SHEREDOM_SUBPROCESS_H_INCLUDED
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum subprocess_option_e {
  subprocess_option_combined_stdout_stderr = 0x1,
  subprocess_option_inherit_environment = 0x2,
  subprocess_option_enable_async = 0x4,
  subprocess_option_no_window = 0x8,
  subprocess_option_search_user_path = 0x10
};
#define SUBPROCESS_NULL NULL

struct subprocess_s {
  FILE *stdin_file, *stdout_file, *stderr_file;
  char *input, *output;
  size_t input_size, output_size;
  const char *const *argv;
  int status;
  int ran;
};

// Runs mksprite on `input` with `argv`; returns its exit status and leaves
// its output (malloc'd) in *output.
EM_JS(int, d64_run_mksprite, (const char *const *argv, const char *input, size_t size,
                             char **output, size_t *output_size), {
  const args = [];
  for (let i = 1;; i++) {
    const at = HEAPU32[(argv >> 2) + i];
    if (!at) break;
    args.push(UTF8ToString(at));
  }
  const result = Module.d64RunMksprite(args, HEAPU8.slice(input, input + size));
  const bytes = result.output;
  const at = _malloc(Math.max(1, bytes.length));
  HEAPU8.set(bytes, at);
  HEAPU32[output >> 2] = at;
  HEAPU32[output_size >> 2] = bytes.length;
  return result.status;
});

static int subprocess_create(const char *const argv[], int options, struct subprocess_s *p) {
  (void)options;
  memset(p, 0, sizeof(*p));
  p->argv = argv;
  p->stdin_file = open_memstream(&p->input, &p->input_size);
  return p->stdin_file ? 0 : -1;
}
static FILE *subprocess_stdin(const struct subprocess_s *p) { return p->stdin_file; }
static void subprocess_run(struct subprocess_s *p) {
  if (p->ran)
    return;
  p->ran = 1;
  // mkfont closes stdin before reading stdout; open_memstream finalizes then.
  p->status = d64_run_mksprite(p->argv, p->input, p->input_size, &p->output, &p->output_size);
  p->stdout_file = fmemopen(p->output, p->output_size ? p->output_size : 1, "rb");
  if (p->stdout_file && !p->output_size)
    fgetc(p->stdout_file); // an empty stream
  p->stderr_file = fmemopen((void *)"", 1, "rb");
  fgetc(p->stderr_file);
}
static FILE *subprocess_stdout(const struct subprocess_s *cp) {
  struct subprocess_s *p = (struct subprocess_s *)cp;
  subprocess_run(p);
  return p->stdout_file;
}
static FILE *subprocess_stderr(const struct subprocess_s *cp) {
  struct subprocess_s *p = (struct subprocess_s *)cp;
  subprocess_run(p);
  return p->stderr_file;
}
static int subprocess_join(struct subprocess_s *p, int *status) {
  subprocess_run(p);
  if (status)
    *status = p->status;
  return 0;
}
static int subprocess_destroy(struct subprocess_s *p) {
  if (p->stdout_file)
    fclose(p->stdout_file);
  if (p->stderr_file)
    fclose(p->stderr_file);
  free(p->input);
  free(p->output);
  return 0;
}
#endif
