#ifndef DIRECTOR64_ANDROID_JSON_H
#define DIRECTOR64_ANDROID_JSON_H
// A small JSON tokenizer for the player's own files: pack indexes, the
// package manifest and the video and sound headers. Tokens point into the
// text; nothing is copied until a string is read.
#include <stdbool.h>
#include <stddef.h>

typedef enum { JSON_OBJECT = 1, JSON_ARRAY, JSON_STRING, JSON_PRIMITIVE } json_type_t;

typedef struct {
  json_type_t type;
  size_t start, end;
  // Members of an object (keys and values both count) or items of an array.
  unsigned size;
} json_token_t;

typedef struct {
  const char *text;
  json_token_t *tokens;
  unsigned count;
} json_t;

// Parses `length` bytes of text; false on malformed input.
bool json_parse(json_t *, const char *text, size_t length);
void json_free(json_t *);
// The token after `index` and everything nested in it.
unsigned json_next(const json_t *, unsigned index);
// The value of `key` in the object at `index`, or 0 when absent.
unsigned json_get(const json_t *, unsigned index, const char *key);
// Item `n` of the array at `index`, or 0.
unsigned json_item(const json_t *, unsigned index, unsigned n);
double json_number(const json_t *, unsigned index);
// The string's unescaped UTF-8, truncated to `cap` - 1 bytes; its length.
size_t json_string(const json_t *, unsigned index, char *out, size_t cap);
bool json_equals(const json_t *, unsigned index, const char *text);
#endif
