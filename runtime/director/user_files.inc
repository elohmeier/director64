// Modal file services use an interpreter continuation, so native dialogs do
// not return invented cancellation while the actual player is choosing a file.
#define USER_FILE_SLOTS 8
#define USER_FILE_LIMIT (16u * 1024u * 1024u)
#define USER_MEMORY_LIMIT (64u * 1024u * 1024u)
typedef struct {
  unsigned id, refs, size, width, height;
  uint8_t *data;
  uint32_t *pixels;
  bool exists, writable;
  char name[128];
} user_file_t;
static user_file_t user_files[USER_FILE_SLOTS];
static dg_user_request_t user_request;
static dg_runtime_t *user_owner;
static unsigned user_serial;
static bool user_done, user_cancelled, user_success;
static char user_path[192], user_filter[256];
#if DG_D10
static uint32_t *user_print_pixels;
static int user_print_frame[4], user_print_margin[4];
static int user_print_width = 595, user_print_height = 842;
static char user_print_name[128];
static dg_runtime_t *user_print_owner;
#endif

static void user_file_free(user_file_t *file) {
  free(file->data);
  free(file->pixels);
  memset(file, 0, sizeof(*file));
}
void dg_user_reset(void) {
  for (unsigned i = 0; i < USER_FILE_SLOTS; i++)
    user_file_free(&user_files[i]);
  memset(&user_request, 0, sizeof(user_request));
  user_owner = NULL;
  user_done = user_cancelled = user_success = false;
  user_path[0] = user_filter[0] = 0;
#if DG_D10
  free(user_print_pixels);
  user_print_pixels = NULL;
  user_print_owner = NULL;
#endif
}
const dg_user_request_t *dg_user_request(void) {
  return user_request.kind ? &user_request : NULL;
}
static user_file_t *user_file_id(unsigned id) {
  for (unsigned i = 0; i < USER_FILE_SLOTS; i++)
    if (user_files[i].id == id)
      return &user_files[i];
  return NULL;
}
#if DG_MODERN
static user_file_t *user_file_path(const char *path) {
  static const char prefix[] = "C:\\Director64Files\\";
  unsigned i = 0;
  while (prefix[i] && path[i] &&
         tolower((unsigned char)prefix[i]) == tolower((unsigned char)path[i]))
    i++;
  if (prefix[i])
    return NULL;
  unsigned id = 0, digits = 0;
  while (path[i] >= '0' && path[i] <= '9') {
    if (++digits > 9 || id > 100000000u)
      return NULL;
    id = id * 10u + (unsigned)(path[i++] - '0');
  }
  if (!digits || path[i++] != '\\' || !path[i])
    return NULL;
  for (; path[i]; i++)
    if (path[i] == '\\' || path[i] == '/' || path[i] == ':' ||
        (unsigned char)path[i] < 32)
      return NULL;
  return user_file_id(id);
}
#endif
static bool user_name(char out[128], const char *filename) {
  if (!filename || !*filename || strlen(filename) >= 128)
    return false;
  if (!strcmp(filename, ".") || !strcmp(filename, ".."))
    return false;
  for (unsigned i = 0; filename[i]; i++) {
    unsigned char ch = (unsigned char)filename[i];
    if (ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == 127)
      return false;
  }
  strcpy(out, filename);
  return true;
}
bool dg_user_reply(unsigned id, const char *name, const uint8_t *data,
                   unsigned size, const uint32_t *pixels, unsigned width,
                   unsigned height) {
  if (id != user_request.id || user_done ||
      (user_request.kind != DG_USER_OPEN && user_request.kind != DG_USER_SAVE))
    return false;
  user_file_t *file = user_file_id(id);
  if (!file || !user_name(file->name, name) || size > USER_FILE_LIMIT ||
      (size && !data) || width > 4096 || height > 4096 ||
      ((width || height) && (!width || !height || !pixels)) ||
      (uint64_t)width * height * 4u > 32u * 1024u * 1024u)
    return false;
  uint64_t bytes = (uint64_t)width * height * 4u + size;
  for (unsigned i = 0; i < USER_FILE_SLOTS; i++)
    if (&user_files[i] != file)
      bytes += user_files[i].size +
               (uint64_t)user_files[i].width * user_files[i].height * 4u;
  for (unsigned i = 0; bytes > USER_MEMORY_LIMIT && i < USER_FILE_SLOTS; i++) {
    user_file_t *old = &user_files[i];
    if (old == file || old->refs)
      continue;
    bytes -= old->size + (uint64_t)old->width * old->height * 4u;
    user_file_free(old);
  }
  if (bytes > USER_MEMORY_LIMIT)
    return false;
  uint8_t *copy = size ? malloc(size) : NULL;
  uint32_t *rgba = width ? malloc((size_t)width * height * 4u) : NULL;
  if ((size && !copy) || (width && !rgba)) {
    free(copy);
    free(rgba);
    return false;
  }
  if (size)
    memcpy(copy, data, size);
  if (width)
    memcpy(rgba, pixels, (size_t)width * height * 4u);
  free(file->data);
  free(file->pixels);
  file->data = copy;
  file->pixels = rgba;
  file->size = size;
  file->width = width;
  file->height = height;
  file->exists = user_request.kind == DG_USER_OPEN;
  file->writable = user_request.kind == DG_USER_SAVE;
  snprintf(user_path, sizeof(user_path), "C:\\Director64Files\\%u\\%s", id,
           file->name);
  user_done = true;
  user_success = true;
  user_request.kind = 0;
  return true;
}
bool dg_user_cancel(unsigned id) {
  if (!user_request.kind || id != user_request.id)
    return false;
  user_cancelled = user_done = true;
  user_success = false;
  user_path[0] = 0;
  user_request.kind = 0;
  return true;
}
bool dg_user_complete(unsigned id, bool success) {
  if (id != user_request.id || (user_request.kind != DG_USER_EXPORT &&
                                user_request.kind != DG_USER_PRINT))
    return false;
  user_done = true;
  user_success = success;
  user_request.kind = 0;
  return true;
}
static lv_flow_t user_wait_step(lv_runtime_t *r, lv_frame_t *f) {
  if (!user_done)
    return LV_YIELD;
  if (lv_integer(r, f->locals[1]))
    r->result = lv_num(user_success ? 0 : -37);
  else
    r->result =
        user_cancelled
            ? (lv_integer(r, f->locals[2]) ? lv_num(0) : lv_text(r, "", false))
            : lv_text(r, user_path, false);
  user_owner = NULL;
#if DG_MODERN
  if (lv_integer(r, f->locals[1]) && lv_integer(r, f->locals[2]) > 1) {
    unsigned index = (unsigned)lv_integer(r, f->locals[2]) - 2;
    if (index < 4)
      ((dg_runtime_t *)r->context)->files[index].status =
          user_success ? 0 : -37;
  }
#endif
  return LV_RETURN;
}
static const lv_handler_t user_wait_handler = {.name = "wait",
                                               .kind = "MovieScript",
                                               .arguments = 3,
                                               .locals = 3,
                                               .step = user_wait_step};
static const lv_movie_t user_wait_movie = {
    .name = "<player file dialog>", .count = 1, .handlers = &user_wait_handler};
static bool user_wait(dg_runtime_t *d, bool completion, bool numeric_cancel) {
  lv_t args[] = {lv_num(user_request.id), lv_num(completion),
                 lv_num(numeric_cancel)};
  return lv_start(d->values, &user_wait_movie, "wait", 0, 3, args);
}
static user_file_t *user_new(dg_runtime_t *d, unsigned kind, const char *title,
                             const char *name, const char *filter) {
  if (user_request.kind || user_owner) {
    lv_fail(d->values, "a player file operation is already active");
    return NULL;
  }
  user_file_t *file = NULL;
  for (unsigned i = 0; i < USER_FILE_SLOTS; i++)
    if (!user_files[i].refs && (!file || user_files[i].id < file->id))
      file = &user_files[i];
  if (!file) {
    lv_fail(d->values, "player file handle capacity exceeded");
    return NULL;
  }
  user_file_free(file);
  if (++user_serial >= 1000000000u)
    user_serial = 1;
  file->id = user_serial;
  memset(&user_request, 0, sizeof(user_request));
  user_request.id = file->id;
  user_request.kind = kind;
  snprintf(user_request.title, sizeof(user_request.title), "%s",
           title ? title : "");
  snprintf(user_request.name, sizeof(user_request.name), "%s",
           name ? name : "");
  snprintf(user_request.filter, sizeof(user_request.filter), "%s",
           filter ? filter : "");
  user_owner = d;
  user_done = user_cancelled = user_success = false;
  user_path[0] = 0;
  return file;
}
#if DG_MODERN
static void user_export(dg_runtime_t *d, user_file_t *file, unsigned kind,
                        const char *title) {
  memset(&user_request, 0, sizeof(user_request));
  user_request.id = file->id;
  user_request.kind = kind;
  user_request.data = file->data;
  user_request.size = file->size;
  snprintf(user_request.title, sizeof(user_request.title), "%s", title);
  snprintf(user_request.name, sizeof(user_request.name), "%s", file->name);
  user_owner = d;
  user_done = user_cancelled = user_success = false;
}
static bool user_file_io(dg_runtime_t *d, const char *name, unsigned argc,
                         const lv_t *a, lv_t *out, bool *yield) {
  lv_runtime_t *r = d->values;
  if (!argc || lv_type(a[0]) != LV_FILE || lv_id(a[0]) < 0 || lv_id(a[0]) >= 4)
    return false;
  dg_file_t *f = &d->files[lv_id(a[0])];
  if (!f->used)
    return false;
  if (eq_full(name, "setfiltermask")) {
    snprintf(user_filter, sizeof(user_filter), "%s",
             argc > 1 ? lv_cstr(r, a[1]) : "");
    return true;
  }
  if (eq_full(name, "displayopen") || eq_full(name, "displaysave")) {
    bool save = eq_full(name, "displaysave");
    if (user_new(d, save ? DG_USER_SAVE : DG_USER_OPEN,
                 save && argc > 1 ? lv_cstr(r, a[1]) : "Open file",
                 save && argc > 2 ? lv_cstr(r, a[2]) : "", user_filter) &&
        user_wait(d, false, true))
      *yield = true;
    return true;
  }
  bool create = eq_full(name, "createfile"), open = eq_full(name, "openfile");
  user_file_t *file = NULL;
  if ((create || open) && argc > 1)
    file = user_file_path(lv_cstr(r, a[1]));
  if (file) {
    if (f->external) {
      user_file_t *old = user_file_id(f->file);
      if (old && old->refs)
        old->refs--;
    }
    f->external = true;
    f->file = file->id;
    file->refs++;
    snprintf(d->file_names[lv_id(a[0])], sizeof(d->file_names[0]), "%s",
             lv_cstr(r, a[1]));
    f->position = 0;
    f->dirty = false;
    f->write = create || argc < 3 || lv_integer(r, a[2]) != 1;
    if (create && file->writable) {
      free(file->data);
      file->data = NULL;
      file->size = 0;
      file->exists = true;
    }
    f->opened = !create && file->exists;
    f->length = file->size;
    f->status = (file->exists && (!create || file->writable)) ? 0 : -37;
    *out = lv_num(f->status);
    return true;
  }
  if ((create || open) && f->external) {
    user_file_t *old = user_file_id(f->file);
    if (old && old->refs)
      old->refs--;
    f->external = false;
  }
  if (!f->external)
    return false;
  file = user_file_id(f->file);
  if (!file) {
    lv_fail(r, "expired player file handle");
    return true;
  }
  if (eq_full(name, "closefile") || eq_full(name, "dispose")) {
    bool dirty = f->dirty;
    f->dirty = f->opened = false;
    if (file->refs)
      file->refs--;
    f->external = false;
    if (eq_full(name, "dispose"))
      f->used = false;
    *out = lv_num(f->status);
    if (dirty && file->writable) {
      user_export(d, file, DG_USER_EXPORT, "Save file");
      lv_t args[] = {lv_num(user_request.id), lv_num(1),
                     lv_num(lv_id(a[0]) + 2)};
      if (lv_start(r, &user_wait_movie, "wait", 0, 3, args))
        *yield = true;
    }
    return true;
  }
  if (eq_full(name, "status")) {
    *out = lv_num(f->status);
    return true;
  }
  if (eq_full(name, "getlength")) {
    *out = lv_num(file->size);
    return true;
  }
  if (eq_full(name, "getposition")) {
    *out = lv_num(f->position);
    return true;
  }
  if (eq_full(name, "setposition")) {
    long pos = argc > 1 ? lv_integer(r, a[1]) : -1;
    if (pos < 0 || (unsigned long)pos > file->size)
      f->status = -37;
    else
      f->position = (unsigned)pos;
    *out = lv_num(f->status);
    return true;
  }
  if (eq_full(name, "writestring")) {
    const char *s = argc > 1 ? lv_cstr(r, a[1]) : "";
    size_t n = argc > 1 ? lv_byte_count(r, a[1]) : 0;
    if (!file->writable || !f->opened || !f->write ||
        n > USER_FILE_LIMIT - f->position) {
      f->status = -37;
      *out = lv_num(-37);
      return true;
    }
    unsigned end = f->position + (unsigned)n;
    uint64_t used = end;
    for (unsigned i = 0; i < USER_FILE_SLOTS; i++)
      used += (uint64_t)user_files[i].width * user_files[i].height * 4u +
              (&user_files[i] == file ? 0 : user_files[i].size);
    if (used > USER_MEMORY_LIMIT) {
      f->status = -37;
      *out = lv_num(-37);
      return true;
    }
    if (end > file->size) {
      uint8_t *p = realloc(file->data, end);
      if (!p && end) {
        lv_fail(r, "player file allocation failed");
        return true;
      }
      file->data = p;
      file->size = end;
    }
    if (n)
      memcpy(file->data + f->position, s, n);
    f->position = end;
    f->length = file->size;
    f->dirty = true;
    *out = lv_num(0);
    return true;
  }
  if (eq_full(name, "readfile") || eq_full(name, "readline") ||
      eq_full(name, "readword") || eq_full(name, "readchar")) {
    unsigned begin = f->position, end = begin;
    if (f->opened)
      while (end < file->size) {
        unsigned char c = file->data[end];
        if (eq_full(name, "readchar")) {
          end++;
          break;
        }
        if (!eq_full(name, "readfile") &&
            (c == '\r' || c == '\n' || (eq_full(name, "readword") && c == ' ')))
          break;
        end++;
      }
    unsigned n = end - begin;
    if (n >= sizeof(f->data)) {
      lv_script_fail(r, "selected file text exceeds FileIO string capacity");
      return true;
    }
    if (n)
      memcpy(f->data, file->data + begin, n);
    f->data[n] = 0;
    f->position = end;
    if (!eq_full(name, "readchar") && !eq_full(name, "readfile") &&
        f->position < file->size) {
      if (file->data[f->position] == '\r')
        f->position++;
      if (f->position < file->size &&
          (file->data[f->position] == '\n' || file->data[f->position] == ' '))
        f->position++;
    }
    // Host document adapters normalize CAR text to UTF-8. Binary readChar
    // and raster data retain exact bytes, including embedded zeroes.
    size_t name_length = strlen(file->name);
    bool car =
        name_length >= 4 && eq_full(file->name + name_length - 4, ".car");
    *out = car && !file->width && !file->height && !eq_full(name, "readchar") &&
                   !memchr(f->data, 0, n)
               ? lv_text(r, f->data, false)
               : lv_bytes(r, f->data, n);
    return true;
  }
  return false;
}
#endif

#if DG_D10
// Windows DIB export is deliberately independent of the host's image encoder.
// The same bytes can be downloaded, written through Android SAF, or printed.
static void user_le16(uint8_t *p, unsigned n) {
  p[0] = (uint8_t)n;
  p[1] = (uint8_t)(n >> 8);
}
static void user_le32(uint8_t *p, unsigned n) {
  for (unsigned i = 0; i < 4; i++)
    p[i] = (uint8_t)(n >> (8 * i));
}
static bool user_bitmap(user_file_t *file, const uint32_t *pixels,
                        unsigned width, unsigned height) {
  uint64_t stride = ((uint64_t)width * 3u + 3u) & ~3ull,
           size = 54u + stride * height;
  if (!width || !height || width > 4096 || height > 4096 ||
      size > USER_FILE_LIMIT)
    return false;
  uint64_t retained = size;
  for (unsigned i = 0; i < USER_FILE_SLOTS; i++)
    retained += (uint64_t)user_files[i].width * user_files[i].height * 4u +
                (&user_files[i] == file ? 0 : user_files[i].size);
  for (unsigned i = 0; retained > USER_MEMORY_LIMIT && i < USER_FILE_SLOTS;
       i++) {
    user_file_t *old = &user_files[i];
    if (old == file || old->refs)
      continue;
    retained -= old->size + (uint64_t)old->width * old->height * 4u;
    user_file_free(old);
  }
  if (retained > USER_MEMORY_LIMIT)
    return false;
  uint8_t *data = calloc(1, (size_t)size);
  if (!data)
    return false;
  data[0] = 'B';
  data[1] = 'M';
  user_le32(data + 2, (unsigned)size);
  user_le32(data + 10, 54);
  user_le32(data + 14, 40);
  user_le32(data + 18, width);
  user_le32(data + 22, height);
  user_le16(data + 26, 1);
  user_le16(data + 28, 24);
  user_le32(data + 34, (unsigned)(stride * height));
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      uint32_t p = pixels[(size_t)y * width + x];
      unsigned alpha = p >> 24;
      uint8_t *to = data + 54 + (height - 1 - y) * stride + 3 * x;
      for (unsigned c = 0; c < 3; c++)
        to[2 - c] = (uint8_t)((((p >> (8 * c)) & 255u) * alpha +
                               255u * (255u - alpha) + 127u) /
                              255u);
    }
  free(file->data);
  file->data = data;
  file->size = (unsigned)size;
  file->exists = true;
  return true;
}
static bool user_rect(dg_runtime_t *d, lv_t value, int out[4]) {
  if (lv_type(value) != LV_LIST || lv_count(d->values, value) != 4) {
    lv_script_fail(d->values, "document rectangle required");
    return false;
  }
  for (unsigned i = 0; i < 4; i++) {
    out[i] = (int)lv_integer(d->values, lv_at(d->values, value, i + 1));
    if (out[i] < -65535 || out[i] > 65535) {
      lv_script_fail(d->values, "document rectangle exceeds bounds");
      return false;
    }
  }
  if (out[2] < out[0] || out[3] < out[1]) {
    lv_script_fail(d->values, "invalid document rectangle");
    return false;
  }
  return true;
}
static void user_print_blit(const dg_pixel_image_t *image, const int src[4],
                            const int dst[4]) {
  int w = dst[2] - dst[0], h = dst[3] - dst[1];
  if (w <= 0 || h <= 0)
    return;
  int first_x = dst[0] > -user_print_margin[0] ? dst[0] : -user_print_margin[0];
  int first_y = dst[1] > -user_print_margin[1] ? dst[1] : -user_print_margin[1];
  int last_x = dst[2] < user_print_width - user_print_margin[0]
                   ? dst[2]
                   : user_print_width - user_print_margin[0];
  int last_y = dst[3] < user_print_height - user_print_margin[1]
                   ? dst[3]
                   : user_print_height - user_print_margin[1];
  for (int y = first_y; y < last_y; y++)
    for (int x = first_x; x < last_x; x++) {
      int px = x + user_print_margin[0], py = y + user_print_margin[1];
      int sx = src[0] + (int)((int64_t)(x - dst[0]) * (src[2] - src[0]) / w);
      int sy = src[1] + (int)((int64_t)(y - dst[1]) * (src[3] - src[1]) / h);
      if (px < 0 || py < 0 || px >= user_print_width ||
          py >= user_print_height || sx < 0 || sy < 0 ||
          (unsigned)sx >= image->width || (unsigned)sy >= image->height)
        continue;
      uint32_t p = image->pixels[(size_t)sy * image->width + (unsigned)sx],
               old = user_print_pixels[py * user_print_width + px];
      unsigned alpha = p >> 24;
      uint32_t blend = 0xff000000u;
      for (unsigned c = 0; c < 3; c++)
        blend |= ((((p >> (8 * c)) & 255u) * alpha +
                   ((old >> (8 * c)) & 255u) * (255u - alpha) + 127u) /
                  255u)
                 << (8 * c);
      user_print_pixels[py * user_print_width + px] = blend;
    }
}
static bool user_print_call(dg_runtime_t *d, const char *name, unsigned argc,
                            const lv_t *a, lv_t *out, bool *yield) {
  lv_runtime_t *r = d->values;
  if (eq_full(name, "xtra") && argc == 1 &&
      (lv_type(a[0]) == LV_STRING || lv_type(a[0]) == LV_SYMBOL) &&
      eq_full(lv_cstr(r, a[0]), "printomatic")) {
    *out = lv_make(LV_XTRA, 6);
    return true;
  }
  if (!argc || lv_type(a[0]) != LV_XTRA || lv_id(a[0]) != 6)
    return false;
  if (eq_full(name, "new")) {
    if (user_request.kind || user_owner) {
      lv_script_fail(r, "a document operation is already active");
      return true;
    }
    free(user_print_pixels);
    user_print_pixels = malloc(595u * 842u * 4u);
    if (!user_print_pixels) {
      lv_fail(r, "print page allocation failed");
      return true;
    }
    for (unsigned i = 0; i < 595u * 842u; i++)
      user_print_pixels[i] = UINT32_MAX;
    user_print_owner = d;
    user_print_width = 595;
    user_print_height = 842;
    strcpy(user_print_name, "Document.bmp");
    memset(user_print_margin, 0, sizeof(user_print_margin));
    memcpy(user_print_frame, (int[]){0, 0, 595, 842}, sizeof(user_print_frame));
    *out = a[0];
    return true;
  }
  if (eq_full(name, "register")) {
    *out = lv_num(0);
    return true;
  }
  if (eq_full(name, "getversion")) {
    *out = lv_text(r, "Director64 portable printing", false);
    return true;
  }
  if (!user_print_pixels || user_print_owner != d) {
    lv_script_fail(r, "print document is not open");
    return true;
  }
  if (eq_full(name, "reset")) {
    user_print_width = 595;
    user_print_height = 842;
    memset(user_print_margin, 0, sizeof(user_print_margin));
    memcpy(user_print_frame, (int[]){0, 0, 595, 842}, sizeof(user_print_frame));
    for (unsigned i = 0; i < 595u * 842u; i++)
      user_print_pixels[i] = UINT32_MAX;
    return true;
  }
  if (eq_full(name, "setlandscapemode") && argc == 2) {
    bool landscape = lv_truth(r, a[1]);
    user_print_width = landscape ? 842 : 595;
    user_print_height = landscape ? 595 : 842;
    memcpy(user_print_frame, (int[]){0, 0, user_print_width, user_print_height},
           sizeof(user_print_frame));
    return true;
  }
  if (eq_full(name, "setdocumentname") && argc == 2) {
    snprintf(user_print_name, sizeof(user_print_name), "%.120s.bmp",
             lv_cstr(r, lv_text_utf8(r, a[1])));
    return true;
  }
  if (eq_full(name, "setprogressloc"))
    return true; // The host owns its dialog placement.
  if (eq_full(name, "dojobsetup")) {
    *out = lv_num(1);
    return true;
  } // The actual print request opens native setup.
  if (eq_full(name, "setprintablemargins")) {
    memcpy(user_print_margin, (int[]){20, 20, 20, 20},
           sizeof(user_print_margin));
    return true;
  }
  if (eq_full(name, "setmargins") && argc == 2)
    return user_rect(d, a[1], user_print_margin);
  if (eq_full(name, "newpage")) {
    for (unsigned i = 0; i < 595u * 842u; i++)
      user_print_pixels[i] = UINT32_MAX;
    return true;
  }
  if (eq_full(name, "getpagewidth")) {
    *out =
        lv_num(user_print_width - user_print_margin[0] - user_print_margin[2]);
    return true;
  }
  if (eq_full(name, "getpageheight")) {
    *out =
        lv_num(user_print_height - user_print_margin[1] - user_print_margin[3]);
    return true;
  }
  if (eq_full(name, "newframe") && argc >= 2)
    return user_rect(d, a[1], user_print_frame);
  if (eq_full(name, "drawstagepicture") && argc == 3) {
    int dst[4], src[4];
    if (!user_rect(d, a[1], dst) || !user_rect(d, a[2], src))
      return true;
    dg_image_entry_t *im = imaging_read(d, 0);
    if (im)
      user_print_blit(&im->image, src, dst);
    return true;
  }
  if (eq_full(name, "drawpicture") && argc == 3 && lv_type(a[1]) == LV_MEMBER) {
    int dst[4];
    if (!user_rect(d, a[2], dst))
      return true;
    dg_image_entry_t *im = imaging_read(d, (uint32_t)lv_id(a[1]));
    if (im)
      user_print_blit(
          &im->image,
          (int[]){0, 0, (int)im->image.width, (int)im->image.height}, dst);
    return true;
  }
  if (eq_full(name, "append") && argc >= 2 && lv_type(a[1]) == LV_MEMBER) {
    dg_image_entry_t *im = imaging_read(d, (uint32_t)lv_id(a[1]));
    if (im) {
      int dst[4];
      memcpy(dst, user_print_frame, sizeof(dst));
      int available_w = dst[2] - dst[0], available_h = dst[3] - dst[1];
      unsigned w = im->image.width, h = im->image.height;
      if (w && h && available_w > 0 && available_h > 0) {
        double scale =
            fmin(1.0, fmin((double)available_w / w, (double)available_h / h));
        dst[2] = dst[0] + (int)(w * scale);
        dst[3] = dst[1] + (int)(h * scale);
        user_print_blit(&im->image, (int[]){0, 0, (int)w, (int)h}, dst);
      }
    }
    return true;
  }
  if (eq_full(name, "drawline") && argc == 3) {
    if (lv_count(r, a[1]) != 2 || lv_count(r, a[2]) != 2) {
      lv_script_fail(r, "print line points required");
      return true;
    }
    int points[4];
    for (unsigned i = 0; i < 4; i++) {
      long coordinate = lv_integer(r, lv_at(r, a[1 + i / 2], 1 + i % 2));
      if (coordinate < -65535 || coordinate > 65535) {
        lv_script_fail(r, "print line exceeds bounds");
        return true;
      }
      points[i] = (int)coordinate + user_print_margin[i % 2];
    }
    int x = points[0], y = points[1], x1 = points[2], y1 = points[3];
    int dx = abs(x1 - x), dy = -abs(y1 - y), sx = x < x1 ? 1 : -1,
        sy = y < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
      if (x >= 0 && y >= 0 && x < user_print_width && y < user_print_height)
        user_print_pixels[y * user_print_width + x] = 0xff000000u;
      if (x == x1 && y == y1)
        break;
      int e = 2 * err;
      if (e >= dy) {
        err += dy;
        x += sx;
      }
      if (e <= dx) {
        err += dx;
        y += sy;
      }
    }
    return true;
  }
  if (eq_full(name, "print") || eq_full(name, "printpreview")) {
    user_file_t *file =
        user_new(d, DG_USER_PRINT, "Print document", user_print_name, "");
    if(file && !user_name(file->name,user_print_name)) {
      lv_script_fail(r,"invalid print document name");return true;
    }
    if (file && user_bitmap(file, user_print_pixels, (unsigned)user_print_width,
                            (unsigned)user_print_height)) {
      user_export(d, file, DG_USER_PRINT,
                  eq_full(name, "printpreview") ? "Print preview"
                                                : "Print document");
      if (user_wait(d, true, false))
        *yield = true;
    } else if (file)
      lv_fail(r, "print page encoding failed");
    return true;
  }
  return false;
}
static bool user_imaging_call(dg_runtime_t *d, const char *name, unsigned argc,
                              const lv_t *a, lv_t *out, bool *yield) {
  lv_runtime_t *r = d->values;
  if (eq_full(name, "importfileinto") && argc >= 2 &&
      lv_type(a[0]) == LV_MEMBER) {
    user_file_t *file = user_file_path(lv_cstr(r, a[1]));
    if (!file || !file->pixels) {
      lv_script_fail(r, "selected picture could not be decoded");
      return true;
    }
    uint32_t id = (uint32_t)lv_id(a[0]);
    dg_member_t *m = NULL;
    const dg_member_t *original = dg_member(d, id);
    for (unsigned i = 0; i < d->dynamic_count; i++)
      if (d->dynamic_members[i].member.id == id)
        m = &d->dynamic_members[i].member;
    if (!m) {
      if (d->dynamic_count >= 32) {
        lv_fail(r, "imported cast capacity exceeded");
        return true;
      }
      unsigned index = d->dynamic_count++;
      m = &d->dynamic_members[index].member;
      *m = original ? *original
                    : (dg_member_t){.id = id,
                                    .number = id & 65535u,
                                    .cast = DG_ID_CAST(id),
                                    .name = "",
                                    .text = "",
                                    .asset = ""};
    }
    dg_image_entry_t *im =
        imaging_new(d, (int)file->width, (int)file->height, 32);
    if (!im)
      return true;
    memcpy(im->image.pixels, file->pixels,
           (size_t)file->width * file->height * 4u);
    dg_image_entry_t *old = imaging_member(d, id);
    if (old)
      old->member = 0;
    im->member = id;
    im->image.use_alpha = true;
    imaging_changed(d, im);
    m->type = 1;
    m->width = (uint16_t)file->width;
    m->height = (uint16_t)file->height;
    m->reg_x = (int16_t)(file->width / 2);
    m->reg_y = (int16_t)(file->height / 2);
    *out = a[0];
    return true;
  }
  if (eq_full(name, "stagetofile") && argc >= 5) {
    user_file_t *file = user_file_path(lv_cstr(r, a[4]));
    if (!file || !file->writable) {
      lv_script_fail(r, "picture export requires a selected destination");
      return true;
    }
    int rect[4];
    for (unsigned i = 0; i < 4; i++)
      rect[i] = (int)lv_integer(r, a[i]);
    dg_image_entry_t *stage = imaging_read(d, 0);
    if (!stage)
      return true;
    dg_image_entry_t *im =
        imaging_copy(d, stage, rect[0], rect[1], rect[2], rect[3]);
    if (!im)
      return true;
    if (!user_bitmap(file, im->image.pixels, im->image.width,
                     im->image.height)) {
      lv_script_fail(r, "picture export exceeds image limits");
      return true;
    }
    user_export(d, file, DG_USER_EXPORT, "Save picture");
    if (user_wait(d, true, false))
      *yield = true;
    return true;
  }
  if ((eq_full(name, "deletefile") || eq_full(name, "fx_deletefile")) && argc &&
      user_file_path(lv_cstr(r, a[0]))) {
    *out = lv_num(0);
    return true;
  }
  return false;
}
#endif

static bool user_files_call(dg_runtime_t *d, const char *name, unsigned argc,
                            const lv_t *a, lv_t *out, bool *yield) {
#if DG_MODERN
  if (user_file_io(d, name, argc, a, out, yield))
    return true;
#endif
#if DG_D10
  if ((eq_full(name, "fileexists") || eq_full(name, "fx_fileexists")) && argc) {
    unsigned base = lv_type(a[0]) == LV_XTRA ? 1 : 0;
    user_file_t *file =
        argc > base ? user_file_path(lv_cstr(d->values, a[base])) : NULL;
    if (file) {
      *out = lv_num(eq_full(name, "fileexists") ? (file->exists ? 0 : -1)
                                                : file->exists);
      return true;
    }
  }
  if (user_print_call(d, name, argc, a, out, yield) ||
      user_imaging_call(d, name, argc, a, out, yield))
    return true;
  if (eq_full(name, "deletefile") || eq_full(name, "fx_deletefile") ||
      eq_full(name, "renamefile") || eq_full(name, "fx_renamefile")) {
    unsigned base = argc && lv_type(a[0]) == LV_XTRA ? 1 : 0;
    bool rename = eq_full(name, "renamefile") || eq_full(name, "fx_renamefile");
    bool ok = false;
    if (argc >= base + (rename ? 2u : 1u)) {
      const char *path = lv_cstr(d->values, a[base]);
      ok = rename ? (d->platform.rename_file &&
                     d->platform.rename_file(d->context, path,
                                             lv_cstr(d->values, a[base + 1])))
                  : (d->platform.remove_file &&
                     d->platform.remove_file(d->context, path));
    }
    *out = lv_num(ok ? 0 : -1);
    return true;
  }
#endif
  (void)out;
  if (eq_full(name, "filesaveasdialog") ||
      eq_full(name, "fx_filesaveasdialog") ||
      eq_full(name, "fx_fileopendialog") || eq_full(name, "bagetfilename")) {
    unsigned base = argc && lv_type(a[0]) == LV_XTRA ? 1 : 0;
    bool save =
        !eq_full(name, "fx_fileopendialog") && !eq_full(name, "bagetfilename");
    const char *title = save && argc > base + 2
                            ? lv_cstr(d->values, a[base + 2])
                        : save ? "Save file"
                               : "Open file";
    const char *suggest =
        save && argc > base + 1 ? lv_cstr(d->values, a[base + 1]) : "";
    if (user_new(d, save ? DG_USER_SAVE : DG_USER_OPEN, title, suggest,
                 user_filter) &&
        user_wait(d, false, false))
      *yield = true;
    return true;
  }
  return false;
}
