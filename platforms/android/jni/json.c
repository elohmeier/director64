#include "json.h"
#include <stdlib.h>
#include <string.h>

enum { JSON_DEPTH = 64 };

static size_t skip_space(const char *s, size_t at, size_t length) {
  while (at < length && (s[at] == ' ' || s[at] == '\n' || s[at] == '\r' || s[at] == '\t'))
    at++;
  return at;
}

// One pass over the text, storing tokens when `out` is non-NULL; returns the
// token count, or -1 when the text is not JSON.
static long tokenize(const char *s, size_t length, json_token_t *out) {
  unsigned stack[JSON_DEPTH];
  unsigned depth = 0;
  long count = 0;
  for (size_t at = skip_space(s, 0, length); at < length; at = skip_space(s, at, length)) {
    char c = s[at];
    if (c == ',' || c == ':') {
      at++;
      continue;
    }
    if (c == '}' || c == ']') {
      if (!depth)
        return -1;
      depth--;
      if (out)
        out[stack[depth]].end = at + 1;
      at++;
      continue;
    }
    json_token_t token = {0};
    token.start = at;
    if (c == '{' || c == '[') {
      token.type = c == '{' ? JSON_OBJECT : JSON_ARRAY;
      at++;
    } else if (c == '"') {
      token.type = JSON_STRING;
      at++;
      token.start = at;
      while (at < length && s[at] != '"')
        at += s[at] == '\\' ? 2 : 1;
      if (at >= length)
        return -1;
      token.end = at;
      at++;
    } else {
      token.type = JSON_PRIMITIVE;
      while (at < length && !strchr(" \t\r\n,:]}", s[at]))
        at++;
      token.end = at;
    }
    if (depth && out)
      out[stack[depth - 1]].size++;
    if (out)
      out[count] = token;
    if (token.type == JSON_OBJECT || token.type == JSON_ARRAY) {
      if (depth == JSON_DEPTH)
        return -1;
      stack[depth++] = (unsigned)count;
    }
    count++;
  }
  return depth ? -1 : count;
}

bool json_parse(json_t *json, const char *text, size_t length) {
  memset(json, 0, sizeof(*json));
  long count = tokenize(text, length, NULL);
  if (count <= 0)
    return false;
  json->tokens = calloc((size_t)count, sizeof(json_token_t));
  if (!json->tokens)
    return false;
  tokenize(text, length, json->tokens);
  json->text = text;
  json->count = (unsigned)count;
  return true;
}

void json_free(json_t *json) {
  free(json->tokens);
  memset(json, 0, sizeof(*json));
}

unsigned json_next(const json_t *json, unsigned index) {
  unsigned remaining = 1;
  while (remaining && index < json->count) {
    const json_token_t *t = &json->tokens[index++];
    remaining--;
    if (t->type == JSON_OBJECT || t->type == JSON_ARRAY)
      remaining += t->size;
  }
  return index;
}

unsigned json_get(const json_t *json, unsigned index, const char *key) {
  if (index >= json->count || json->tokens[index].type != JSON_OBJECT)
    return 0;
  unsigned members = json->tokens[index].size, at = index + 1;
  for (unsigned i = 0; i < members && at < json->count; i += 2) {
    unsigned value = at + 1;
    if (json_equals(json, at, key))
      return value < json->count ? value : 0;
    at = json_next(json, value);
  }
  return 0;
}

unsigned json_item(const json_t *json, unsigned index, unsigned n) {
  if (index >= json->count || json->tokens[index].type != JSON_ARRAY ||
      n >= json->tokens[index].size)
    return 0;
  unsigned at = index + 1;
  while (n--)
    at = json_next(json, at);
  return at;
}

double json_number(const json_t *json, unsigned index) {
  if (!index || index >= json->count || json->tokens[index].type != JSON_PRIMITIVE)
    return 0;
  char buffer[40];
  size_t length = json->tokens[index].end - json->tokens[index].start;
  if (length >= sizeof(buffer))
    return 0;
  memcpy(buffer, json->text + json->tokens[index].start, length);
  buffer[length] = 0;
  return strtod(buffer, NULL);
}

static unsigned hex(char c) {
  return c >= '0' && c <= '9' ? (unsigned)(c - '0')
         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
         : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                : 0;
}

size_t json_string(const json_t *json, unsigned index, char *out, size_t cap) {
  if (!cap)
    return 0;
  out[0] = 0;
  if (!index || index >= json->count || json->tokens[index].type != JSON_STRING)
    return 0;
  const char *s = json->text;
  size_t n = 0;
  for (size_t at = json->tokens[index].start; at < json->tokens[index].end; at++) {
    unsigned c = (unsigned char)s[at];
    char utf8[4];
    size_t bytes = 1;
    utf8[0] = (char)c;
    if (c == '\\' && at + 1 < json->tokens[index].end) {
      char e = s[++at];
      utf8[0] = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b'
              : e == 'f' ? '\f' : e;
      if (e == 'u' && at + 4 < json->tokens[index].end) {
        unsigned code = hex(s[at + 1]) << 12 | hex(s[at + 2]) << 8 | hex(s[at + 3]) << 4 |
                        hex(s[at + 4]);
        at += 4;
        if (code < 0x80) {
          utf8[0] = (char)code;
        } else if (code < 0x800) {
          utf8[0] = (char)(0xc0 | code >> 6);
          utf8[1] = (char)(0x80 | (code & 63));
          bytes = 2;
        } else {
          utf8[0] = (char)(0xe0 | code >> 12);
          utf8[1] = (char)(0x80 | ((code >> 6) & 63));
          utf8[2] = (char)(0x80 | (code & 63));
          bytes = 3;
        }
      }
    }
    if (n + bytes >= cap)
      break;
    memcpy(out + n, utf8, bytes);
    n += bytes;
  }
  out[n] = 0;
  return n;
}

bool json_equals(const json_t *json, unsigned index, const char *text) {
  if (index >= json->count || json->tokens[index].type != JSON_STRING)
    return false;
  const json_token_t *t = &json->tokens[index];
  size_t length = strlen(text);
  // Keys are plain here; an escaped key compares by its unescaped form.
  if (memchr(json->text + t->start, '\\', t->end - t->start)) {
    char buffer[256];
    json_string(json, index, buffer, sizeof(buffer));
    return !strcmp(buffer, text);
  }
  return t->end - t->start == length && !memcmp(json->text + t->start, text, length);
}
