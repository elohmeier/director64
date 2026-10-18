// Director/Flash value bridge. Lingo identifiers are case-insensitive;
// names explicitly supplied in getVariable/setVariable strings retain
// ActionScript's versioned spelling rules. MX2004 Scripting Reference,
// getVariable: the default result is a string; FALSE preserves its type.
#include "flash_bridge.h"
static lv_t d10_avm_from_lingo(dg_avm1_t *vm, lv_t value, unsigned depth) {
  lv_runtime_t *r = dg_avm1_values(vm);
  if (depth >= 32) {
    lv_fail(r, "Flash argument nesting exceeds 32 levels");
    return 0;
  }
  unsigned kind = lv_type(value);
  if (kind == LV_VOID)
    return dg_avm1_null();
  if (kind == LV_COLOR)
    return lv_num((uint32_t)lv_id(value));
  if (kind == LV_SYMBOL)
    return lv_text(r, lv_cstr(r, value), false);
  if (kind != LV_LIST && kind != LV_PROPLIST)
    return value;
  if (dg_avm1_owns(vm, value))
    return value;
  lv_t constructor = dg_avm1_get(vm, dg_avm1_globals(vm),
                                 kind == LV_LIST ? "Array" : "Object");
  lv_t object = dg_avm1_object(vm, dg_avm1_get(vm, constructor, "prototype"));
  if (kind == LV_LIST)
    dg_avm1_own_set(vm, object, "length", lv_num(0));
  lv_t held[] = {value, object};
  lv_root_span_t span = {r->external_roots, held, 2};
  r->external_roots = &span;
  unsigned count = lv_count(r, value);
  for (unsigned i = 0; i < count && !r->failed; i++) {
    char key[1024];
    lv_t item;
    if (kind == LV_LIST) {
      snprintf(key, sizeof(key), "%u", i);
      item = lv_at(r, value, i + 1);
    } else {
      const char *name = lv_cstr(r, lv_at(r, value, i * 2 + 1));
      if (strlen(name) >= sizeof(key)) {
        lv_fail(r, "Flash argument property name exceeds 1023 bytes");
        break;
      }
      strcpy(key, name);
      item = lv_at(r, value, i * 2 + 2);
    }
    dg_avm1_set(vm, object, key, d10_avm_from_lingo(vm, item, depth + 1));
  }
  if (kind == LV_LIST) {
    dg_avm1_own_set(vm, object, "length", lv_num(count));
    dg_avm1_flags(vm, object, "length", 1, 0);
  }
  r->external_roots = span.next;
  return object;
}
static lv_t d10_avm_to_lingo(lv_t value) {
  if (lv_type(value) == LV_AVM_BOOLEAN)
    return lv_num(lv_id(value) != 0);
  if (lv_type(value) == LV_AVM_NULL)
    return 0;
  return value;
}
static dg_avm1_t *d10_avm_receiver(dg_runtime_t *d, lv_t receiver,
                                   lv_t *object) {
  if (lv_type(receiver) == LV_PROPLIST) {
    dg_avm1_t *vm = dg_flash_vm_for_object(d, receiver);
    if (vm) {
      *object = receiver;
      return vm;
    }
  }
  if (lv_type(receiver) == LV_SPRITE && lv_id(receiver) > 0 &&
      lv_id(receiver) < DG_SPRITES) {
    const dg_member_t *m =
        dg_member(d, d->sprites[lv_id(receiver)].value.member);
    if (m && m->source_xtra == 1 && m->flash_clip_count) {
      dg_flash_sync(d);
      dg_flash_instance_t *root = dg_flash_root(d, (unsigned)lv_id(receiver));
      if (root) {
        *object = dg_flash_object(root);
        return dg_flash_vm(root);
      }
    }
  }
  return NULL;
}
static lv_t d10_avm_property(dg_runtime_t *d, dg_avm1_t *vm, lv_t object,
                             const char *name) {
  dg_flash_instance_t *instance = dg_flash_object_instance(d, object);
  dg_flash_instance_t *child = dg_flash_named(instance, name, true);
  if (child)
    return dg_flash_object(child);
  char key[1024];
  dg_avm1_external_key(vm, object, name, key, sizeof(key));
  static const char *aliases[] = {
      "htmlText",    "textHeight", "textWidth", "maxChars",
      "wordWrap",    "autoSize",   "multiline", "backgroundColor",
      "borderColor", "embedFonts", "selectable"};
  for (unsigned i = 0; i < sizeof(aliases) / sizeof(*aliases); i++)
    if (eq(name, aliases[i])) {
      snprintf(key, sizeof(key), "%s", aliases[i]);
      break;
    }
  return dg_avm1_get(vm, object, key);
}
static bool d10_avm_property_get(lv_runtime_t *r, lv_t object, const char *name,
                                 lv_t *out) {
  dg_runtime_t *d = r->context;
  dg_avm1_t *vm = dg_flash_vm_for_object(d, object);
  if (!vm)
    return false;
  *out = d10_avm_to_lingo(d10_avm_property(d, vm, object, name));
  return true;
}
static bool d10_avm_property_set(lv_runtime_t *r, lv_t object, const char *name,
                                 lv_t value) {
  dg_runtime_t *d = r->context;
  dg_avm1_t *vm = dg_flash_vm_for_object(d, object);
  if (!vm)
    return false;
  char key[1024];
  dg_avm1_external_key(vm, object, name, key, sizeof(key));
  if (eq(name, "htmlText"))
    strcpy(key, "htmlText");
  dg_avm1_set(vm, object, key, d10_avm_from_lingo(vm, value, 0));
  return true;
}
static bool d10_avm_path(dg_runtime_t *d, dg_avm1_t *vm, lv_t root,
                         const char *path, lv_t *owner, char key[1024]) {
  (void)d;
  *owner = root;
  if (strlen(path) >= 1024) {
    lv_fail(dg_avm1_values(vm), "Flash variable path exceeds 1023 bytes");
    return false;
  }
  char buffer[1024];
  strcpy(buffer, path);
  char *p = buffer;
  while (*p == '/')
    p++;
  for (;;) {
    char *end = strpbrk(p, "./:");
    if (end)
      *end = 0;
    if (!*p) {
      lv_fail(dg_avm1_values(vm), "Flash variable path has empty component");
      return false;
    }
    if (!end) {
      strcpy(key, p);
      return true;
    }
    if (!strcmp(p, "_root") || !strcmp(p, "_level0"))
      *owner = root;
    else if (strcmp(p, "this"))
      *owner = dg_avm1_get(vm, *owner, p);
    if (lv_type(*owner) != LV_PROPLIST)
      return false;
    p = end + 1;
  }
}
static bool d10_avm_call(dg_runtime_t *d, const char *name, unsigned argc,
                         const lv_t *a, lv_t *out) {
  if (!argc)
    return false;
  lv_t object = 0;
  dg_avm1_t *vm = d10_avm_receiver(d, a[0], &object);
  if (!vm)
    return false;
  lv_runtime_t *r = d->values;
  if ((eq(name, "getVariable") || eq(name, "setVariable")) && argc >= 2) {
    lv_t owner = 0;
    char key[1024];
    if (!d10_avm_path(d, vm, object, lv_cstr(r, a[1]), &owner, key)) {
      *out = 0;
      return true;
    }
    if (eq(name, "setVariable")) {
      if (argc < 3) {
        lv_fail(r, "Flash setVariable requires a value");
        return true;
      }
      dg_avm1_set(vm, owner, key, d10_avm_from_lingo(vm, a[2], 0));
      *out = 0;
    } else {
      lv_t value = dg_avm1_get(vm, owner, key);
      *out = argc < 3 || lv_truth(r, a[2]) ? dg_avm1_string(vm, value)
                                           : d10_avm_to_lingo(value);
    }
    return true;
  }
  if ((eq(name, "setFlashProperty") || eq(name, "getFlashProperty")) &&
      argc >= 3) {
    dg_flash_instance_t *instance = dg_flash_object_instance(d, object);
    lv_t target = dg_flash_target(instance, lv_cstr(r, a[1]));
    if (!target) {
      *out = 0;
      return true;
    }
    const char *prop = lv_cstr(r, a[2]);
    char key[128];
    snprintf(key, sizeof(key), "%s%s", *prop == '_' ? "" : "_", prop);
    for (char *c = key; *c; c++)
      *c = (char)tolower((unsigned char)*c);
    if (!strcmp(key, "_posx"))
      strcpy(key, "_x");
    else if (!strcmp(key, "_posy"))
      strcpy(key, "_y");
    if (eq(name, "setFlashProperty")) {
      if (argc < 4) {
        lv_fail(r, "Flash setFlashProperty requires a value");
        return true;
      }
      dg_avm1_set(vm, target, key, d10_avm_from_lingo(vm, a[3], 0));
      *out = 0;
    } else
      *out = d10_avm_to_lingo(dg_avm1_get(vm, target, key));
    return true;
  }
  lv_t function = d10_avm_property(d, vm, object, name);
  if (!dg_avm1_callable(vm, function))
    return false;
  if (argc > 65) {
    lv_fail(r, "Flash call exceeds 64 arguments");
    return true;
  }
  lv_t converted[64] = {0};
  lv_root_span_t span = {r->external_roots, converted, argc - 1};
  r->external_roots = &span;
  for (unsigned i = 1; i < argc && !r->failed; i++)
    converted[i - 1] = d10_avm_from_lingo(vm, a[i], 0);
  lv_t result = 0;
  if (!r->failed)
    dg_avm1_invoke(vm, function, object, argc - 1, converted, 65536, &result);
  r->external_roots = span.next;
  *out = d10_avm_to_lingo(result);
  return true;
}
static const char *flash_lingo_space(const char *p) {
  while (isspace((unsigned char)*p))
    p++;
  return p;
}
static bool flash_lingo_name(const char **source, char *out, unsigned size) {
  const char *p = flash_lingo_space(*source);
  unsigned n = 0;
  if (!isalpha((unsigned char)*p) && *p != '_')
    return false;
  while (isalnum((unsigned char)*p) || *p == '_') {
    if (n + 1 == size)
      return false;
    out[n++] = (char)tolower((unsigned char)*p++);
  }
  out[n] = 0;
  *source = flash_lingo_space(p);
  return true;
}
static bool flash_lingo_literal(lv_runtime_t *r, const char **source,
                                lv_t *value, bool argument) {
  const char *p = flash_lingo_space(*source), *start = p;
  bool quote = false;
  unsigned bracket = 0;
  while (*p) {
    if (*p == '"')
      quote = !quote;
    else if (!quote) {
      if (*p == '[')
        bracket++;
      else if (*p == ']') {
        if (!bracket)
          return false;
        bracket--;
      } else if (!bracket && argument && (*p == ',' || *p == ')'))
        break;
    }
    p++;
  }
  if (quote || bracket || p == start || (size_t)(p - start) >= 1024)
    return false;
  char text[1024];
  memcpy(text, start, (size_t)(p - start));
  text[p - start] = 0;
  *value = lv_literal(r, text);
  if (!*value && !eq(text, "void"))
    return false;
  *source = flash_lingo_space(p);
  return true;
}
bool dg_flash_lingo_url(dg_runtime_t *d, const char *url) {
  if (strncmp(url, "lingo:", 6))
    return false;
  lv_runtime_t *r = d->values;
  const char *p = flash_lingo_space(url + 6);
  char name[128], property[128];
  lv_t owner = 0, value = 0;
  if (!flash_lingo_name(&p, name, sizeof(name)))
    goto unsupported;
  if (*p == '(') {
    // Public reference constructors use the ordinary Director services.
    if (strcmp(name, "sound") && strcmp(name, "sprite") &&
        strcmp(name, "member") && strcmp(name, "field") &&
        strcmp(name, "castlib"))
      goto unsupported;
    lv_t first = 0, second = 0;
    p++;
    if (!flash_lingo_literal(r, &p, &first, true))
      goto unsupported;
    if (*p == ',') {
      p++;
      if (!flash_lingo_literal(r, &p, &second, true))
        goto unsupported;
    }
    if (*p++ != ')')
      goto unsupported;
    owner = lv_reference(r, name, first, second);
    p = flash_lingo_space(p);
    if (*p++ != '.')
      goto unsupported;
    if (!flash_lingo_name(&p, property, sizeof(property)))
      goto unsupported;
    while (*p == '.') {
      owner = lv_get(r, NULL, property, owner);
      p++;
      if (!flash_lingo_name(&p, property, sizeof(property)))
        goto unsupported;
    }
  } else {
    int global = lv_global_id(r, name);
    if (global < 0)
      goto unsupported;
    if (*p != '=')
      goto unsupported;
    p++;
    if (!flash_lingo_literal(r, &p, &value, false) || *p)
      goto unsupported;
    r->globals[global] = value;
    return true;
  }
  if (*p++ != '=' || !flash_lingo_literal(r, &p, &value, false) || *p)
    goto unsupported;
  lv_set(r, NULL, property, owner, value);
  return true;
unsupported:
  if (d->platform.trace)
    d->platform.trace(d->context, url);
  error(d, "unsupported Flash Lingo expression", url + 6);
  return true;
}
