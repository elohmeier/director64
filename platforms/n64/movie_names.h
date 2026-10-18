#ifndef DIRECTOR64_N64_MOVIE_NAMES_H
#define DIRECTOR64_N64_MOVIE_NAMES_H

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Converted registry entries use lower-case filename stems with the final
 * dot changed to an underscore. Preserve actual underscores in names. */
static unsigned n64_movie_candidate(const char *name, const char *const *names,
                                    unsigned count, bool *aliased) {
  char key[192];
  size_t length = strlen(name);
  if (length + 5 >= sizeof(key))
    return 0;
  for (size_t i = 0; i <= length; i++)
    key[i] = (char)tolower((unsigned char)name[i]);
  char *dot = strrchr(key, '.');
  if (!dot) {
    dot = key + length;
    strcpy(dot, ".dxr");
  }
  if (!strcmp(dot, ".dir") || !strcmp(dot, ".dcr"))
    strcpy(dot, ".dxr");
  *dot = '_';
  for (unsigned i = 0; i < count; i++)
    if (!strcmp(names[i], key))
      return i + 1;
  char *base = key;
  for (char *p = strstr(key, "__"); p && p < dot; p = strstr(p + 2, "__"))
    base = p + 2;
  char ending[16];
  if (strlen(dot) >= sizeof(ending))
    return 0;
  strcpy(ending, dot);
  size_t stem_length = (size_t)(dot - base);
  char original[192];
  strcpy(original, base);
  if (dot - base > 8) {
    strcpy(base + 8, ending);
    for (unsigned i = 0; i < count; i++)
      if (!strcmp(names[i], key)) {
        *aliased = true;
        return i + 1;
      }
  }
  /* ISO9660 may also remove punctuation before its eight-character limit
   * (authored Intro_Dach -> INTRODAC). Try only after the exact spelling. */
  unsigned n = 0;
  for (size_t i = 0; i < stem_length && n < 8; i++)
    if (isalnum((unsigned char)original[i]))
      base[n++] = original[i];
  strcpy(base + n, ending);
  for (unsigned i = 0; i < count; i++)
    if (!strcmp(names[i], key)) {
      *aliased = true;
      return i + 1;
    }
  return 0;
}

static unsigned n64_movie_lookup(const char *name, const char *path,
                                 const char *current, const char *const *names,
                                 unsigned count, char *alias,
                                 size_t alias_size) {
  bool aliased = false;
  unsigned id = 0;
  char prefix[96], candidate[192];
  if (alias_size)
    alias[0] = 0;
  const char *base = path, *folder = NULL;
  size_t folder_length = 0;
  for (const char *p = path; *p; p++) {
    if (*p == ':' || *p == '\\' || *p == '/') {
      if (p > base && !(p - base == 1 && *base == '@')) {
        folder = base;
        folder_length = (size_t)(p - base);
      }
      base = p + 1;
    }
  }
  if (folder && folder_length < sizeof(prefix)) {
    for (size_t i = 0; i < folder_length; i++) {
      unsigned char c = (unsigned char)folder[i];
      prefix[i] = (isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
    }
    prefix[folder_length] = 0;
    if (snprintf(candidate, sizeof(candidate), "%s__%s", prefix, name) <
        (int)sizeof(candidate))
      id = n64_movie_candidate(candidate, names, count, &aliased);
    if (id)
      aliased = true;
  }
  const char *split = strstr(current, "__");
  if (!id && split && (size_t)(split - current) < sizeof(prefix)) {
    snprintf(prefix, sizeof(prefix), "%.*s", (int)(split - current), current);
    if (snprintf(candidate, sizeof(candidate), "%s__%s", prefix, name) <
        (int)sizeof(candidate))
      id = n64_movie_candidate(candidate, names, count, &aliased);
    if (id)
      aliased = true;
  }
  if (!id)
    id = n64_movie_candidate(name, names, count, &aliased);
  if (id && aliased && alias_size)
    snprintf(alias, alias_size, "%s%s", name, strchr(name, '.') ? "" : ".dxr");
  return id;
}
#endif
