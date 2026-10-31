#include "director.h"
#include "user_files.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static const lv_movie_t code = {.name = "FILES.DXR"};
static const dg_member_t members[] = {
    {.id = DG_ID(1, 1, 24),
     .number = 24,
     .cast = 1,
     .name = "import",
     .text = "",
     .asset = ""},
    {.id = DG_ID(1, 1, 25),
     .number = 25,
     .cast = 1,
     .type = 3,
     .name = "title",
     .text = "Title",
     .width = 2,
     .height = 2},
};
static const dg_frame_t frames[] = {{0, 0}};
static const dg_movie_t movie = {.code = &code,
                                 .id = 1,
                                 .frame_count = 1,
                                 .frames = frames,
                                 .tempo = 30,
                                 .members = members,
                                 .member_count = 2};
static bool yielded;
static bool read_image(void *ctx, uint32_t member, dg_pixel_image_t *image) {
  (void)ctx;
  (void)member;
  *image = (dg_pixel_image_t){.width = 2,
                              .height = 2,
                              .depth = 32,
                              .pixels = malloc(16),
                              .use_alpha = true};
  assert(image->pixels);
  memcpy(image->pixels,
         (uint32_t[]){0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu}, 16);
  return true;
}
static lv_t invoke(const char *name, unsigned count, const lv_t *args) {
  lv_t result = 0;
  yielded = false;
  assert(values.services.call(&values, name, count, args, &result, &yielded));
  if (values.failed)
    fprintf(stderr, "%s: %s\n", name, values.error);
  assert(!values.failed);
  return result;
}
static lv_t text(const char *s) { return lv_text(&values, s, false); }
static void done(void) {
  assert(lv_run(&values, 100));
  assert(!values.depth);
}
static unsigned number(const uint8_t *p) {
  return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 |
         (unsigned)p[3] << 24;
}
int main(void) {
  dg_init(&director, &values, (dg_platform_t){.read_image = read_image}, NULL,
          NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  director.files[0].used = true;
  lv_t file = lv_make(LV_FILE, 0);
  invoke("displayOpen", 1, &file);
  unsigned id = dg_user_request()->id;
  const uint32_t pixels[] = {0xff123456u, 0xff987654u};
  assert(!dg_user_reply(id, "image.bmp", NULL, 0, pixels, 4097, 1));
  assert(
      dg_user_reply(id, "image.bmp", (const uint8_t *)"BM", 2, pixels, 2, 1));
  done();
  lv_t path = values.result, member = lv_make(LV_MEMBER, DG_ID(1, 1, 24));
  invoke("importFileInto", 2, (lv_t[]){member, path});
  assert(dg_member(&director, DG_ID(1, 1, 24))->type == 1);
  const dg_pixel_image_t *image = dg_member_image(&director, DG_ID(1, 1, 24));
  assert(image && image->width == 2 && image->height == 1 &&
         !memcmp(image->pixels, pixels, 8));

  invoke("FileSaveAsDialog", 3,
         (lv_t[]){text("ignored"), text("image.bmp"), text("Save image")});
  id = dg_user_request()->id;
  assert(dg_user_reply(id, "image.bmp", NULL, 0, NULL, 0, 0));
  done();
  path = values.result;
  invoke("StageToFile", 5,
         (lv_t[]){lv_num(1), lv_num(0), lv_num(2), lv_num(2), path});
  const dg_user_request_t *request = dg_user_request();
  assert(yielded && request && request->kind == DG_USER_EXPORT &&
         request->size == 62);
  assert(number(request->data + 18) == 1 && number(request->data + 22) == 2);
  // Bottom-up BGR rows and DWORD padding preserve the selected crop.
  assert(!memcmp(request->data + 54,
                 (uint8_t[]){255, 255, 255, 0, 0, 255, 0, 0}, 8));
  assert(dg_user_complete(id, true));
  done();

  lv_t xtra = invoke("xtra", 1, (lv_t[]){text("PrintOMatic")});
  assert(lv_type(xtra) == LV_XTRA);
  lv_t doc = invoke("new", 1, &xtra);
  invoke("setMargins", 2, (lv_t[]){doc, lv_rect(&values, 20, 20, 20, 20)});
  assert(lv_integer(&values, invoke("getPageWidth", 1, &doc)) == 555);
  invoke("newFrame", 2, (lv_t[]){doc, lv_rect(&values, 0, 0, 2, 2)});
  // Exercise the Lingo builtin dispatcher, including Xtra append overload.
  lv_frame_t caller = {.movie = &code};
  lv_call(&values, &caller, "append", 2,
          (lv_t[]){doc, lv_make(LV_MEMBER, DG_ID(1, 1, 25))});
  assert(!values.failed);
  invoke("drawLine", 3,
         (lv_t[]){doc, lv_point(&values, 0, 3), lv_point(&values, 2, 3)});
  invoke("print", 1, &doc);
  request = dg_user_request();
  assert(yielded && request && request->kind == DG_USER_PRINT);
  assert(number(request->data + 18) == 595 &&
         number(request->data + 22) == 842);
  unsigned stride = (595 * 3 + 3) & ~3u;
  const uint8_t *red = request->data + 54 + (842 - 1 - 20) * stride + 20 * 3;
  assert(!memcmp(red, (uint8_t[]){0, 0, 255}, 3));
  assert(dg_user_complete(request->id, true));
  done();
  assert(strstr(lv_cstr(&values,invoke("GetVersion",1,&doc)),"Director64"));
  invoke("reset",1,&doc);
  invoke("setLandscapeMode",2,(lv_t[]){doc,lv_num(1)});
  invoke("setPrintableMargins",1,&doc);
  invoke("setDocumentName",2,(lv_t[]){doc,text("Bild")});
  invoke("newPage",1,&doc);
  assert(lv_integer(&values,invoke("getPageWidth",1,&doc))==802);
  assert(lv_integer(&values,invoke("getPageHeight",1,&doc))==555);
  invoke("drawPicture",3,(lv_t[]){doc,lv_make(LV_MEMBER,DG_ID(1,1,25)),
                                 lv_rect(&values,0,0,4,2)});
  invoke("printPreview",1,&doc);
  request=dg_user_request();
  assert(request && request->kind==DG_USER_PRINT && !strcmp(request->name,"Bild.bmp"));
  assert(number(request->data+18)==842 && number(request->data+22)==595);
  stride=(842*3+3)&~3u;
  red=request->data+54+(595-1-20)*stride+20*3;
  assert(!memcmp(red,(uint8_t[]){0,0,255},3));
  assert(dg_user_cancel(request->id));done();
  dg_user_reset();
  dg_images_dispose(&director);
  puts("Image import owns decoded pixels; paint export crops BMP; authored "
       "print commands render a document");
}
