// Independent original DIRAPI/IML32 8.5.1r104 response values: an active native
// film's green(17,199,83) child over RGB(210,164,117), then a black40% shape.
// Parent ink/blend are container properties; child effects see the stage.
#include "compositor.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ID(n) DG_ID(1, 1, n)
static wc_t compositor;
static dg_runtime_t director;
static lv_runtime_t values;
static const uint32_t palette[256] = {0xffffff, 0};
static const lv_movie_t code = {.name = "BACKDROP.DXR"};
static const dg_cast_t cast = {.name = "Internal", .file = 1, .cast = 1};
static const dg_frame_t score[] = {{0, 0}};
static dg_film_layer_t leaf = {.channel = 1,
                               .value = {.member = ID(2),
                                         .type = 16,
                                         .width = 4,
                                         .height = 4,
                                         .stretch = 1,
                                         .fore = 1,
                                         .blend = 100}};
static dg_film_layer_t inner = {.channel = 1,
                                .value = {.member = ID(1),
                                          .type = 16,
                                          .x = 2,
                                          .y = 2,
                                          .width = 4,
                                          .height = 4,
                                          .stretch = 1,
                                          .fore = 1,
                                          .blend = 0}};
static const dg_film_frame_t first = {1, &leaf}, second = {1, &inner};
static const dg_member_t members[] = {{.id = ID(1),
                                       .number = 1,
                                       .cast = 1,
                                       .type = 2,
                                       .width = 4,
                                       .height = 4,
                                       .film_count = 1,
                                       .film_frames = &first},
                                      {.id = ID(2),
                                       .number = 2,
                                       .cast = 1,
                                       .type = 1,
                                       .width = 4,
                                       .height = 4,
                                       .asset = "green.fdi"},
                                      {.id = ID(3),
                                       .number = 3,
                                       .cast = 1,
                                       .type = 1,
                                       .width = 4,
                                       .height = 4,
                                       .asset = "mask.fdi"},
                                      {.id = ID(4),
                                       .number = 4,
                                       .cast = 1,
                                       .type = 2,
                                       .width = 8,
                                       .height = 8,
                                       .film_count = 1,
                                       .film_frames = &second},
                                      {.id = ID(5),
                                       .number = 5,
                                       .cast = 1,
                                       .type = 8,
                                       .shape = 1,
                                       .filled = 1,
                                       .pattern = 1}};
static const dg_movie_t movie = {.id = 1,
                                 .code = &code,
                                 .casts = &cast,
                                 .cast_count = 1,
                                 .members = members,
                                 .member_count = 5,
                                 .frames = score,
                                 .frame_count = 1,
                                 .palette = palette,
                                 .stage_color = 0xd2a475};
static uint32_t read_asset(void *ctx, const char *name, uint8_t *out,
                           uint32_t cap) {
  (void)ctx;
  if (!out)
    return 96;
  assert(cap >= 96);
  memset(out, 0, 96);
  memcpy(out, "FDI2", 4);
  out[7] = 96;
  out[9] = out[11] = 4;
  out[16] = 1;
  out[19] = 32;
  bool mask = !strcmp(name, "mask.fdi");
  for (unsigned i = 32; i < 96; i += 4) {
    out[i] = mask ? 128 : 17;
    out[i + 1] = mask ? 128 : 199;
    out[i + 2] = mask ? 128 : 83;
    out[i + 3] = 255;
  }
  return 96;
}
static void render(void) {
  director.visual_revision++;
  wc_render(&compositor, &director, 0, 0, false, 0);
  if (values.failed)
    fprintf(stderr, "%s\n", values.error);
  assert(!values.failed);
}
static uint32_t rgb(unsigned r, unsigned g, unsigned b) {
  return 0xff000000u | r | g << 8 | b << 16;
}
static void place(unsigned member) {
  director.sprites[1] = (dg_sprite_t){.value = {.member = ID(member),
                                                .type = 16,
                                                .x = 10,
                                                .y = 20,
                                                .width = 8,
                                                .height = 8,
                                                .stretch = 1,
                                                .fore = 1,
                                                .blend = 0,
                                                .ink = 9},
                                      .visible = true,
                                      .stretch = true};
  dg_sprite_changed(&director, 1);
}
// Independent double-precision inverse of an explicitly specified bilinear
// surface. This reference never calls the renderer's transform-stack helper.
static bool inverse(const float q[8], double x, double y, double *u,
                    double *v) {
  double a = q[2] - q[0], b = q[6] - q[0], c = q[0] - q[2] + q[4] - q[6];
  double d = q[3] - q[1], e = q[7] - q[1], f = q[1] - q[3] + q[5] - q[7];
  double p = .5, t = .5;
  for (unsigned i = 0; i < 20; i++) {
    double rx = q[0] + a * p + b * t + c * p * t - x,
           ry = q[1] + d * p + e * t + f * p * t - y;
    double j0 = a + c * t, j1 = b + c * p, j2 = d + f * t, j3 = e + f * p,
           det = j0 * j3 - j1 * j2;
    if (fabs(det) < 1e-10)
      return false;
    p -= (rx * j3 - ry * j1) / det;
    t -= (ry * j0 - rx * j2) / det;
  }
  *u = p;
  *v = t;
  return p >= 0 && t >= 0 && p < 1 && t < 1;
}
int main(void) {
  wc_init(&compositor, (wc_host_t){.read = read_asset}, 4 * 1024 * 1024);
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  place(1);
  director.sprites[2] = (dg_sprite_t){.value = {.member = ID(5),
                                                .type = 16,
                                                .width = WC_WIDTH,
                                                .height = WC_HEIGHT,
                                                .stretch = 1,
                                                .fore = 1,
                                                .blend = 40,
                                                .thickness = DG_HAS_BLEND},
                                      .visible = true,
                                      .stretch = true};
  dg_sprite_changed(&director, 2);
  const struct {
    unsigned ink, r, g, b;
  } original[] = {{39, 10, 98, 49}, {37, 126, 119, 70}, {33, 136, 153, 120}};
  for (unsigned i = 0; i < sizeof(original) / sizeof(*original); i++) {
    leaf.value.ink = original[i].ink;
    for (unsigned parent_ink = 0; parent_ink <= 9; parent_ink++) {
      director.sprites[1].value.ink = parent_ink;
      render();
      assert(compositor.scene[22 * WC_WIDTH + 12] ==
             rgb(original[i].r, original[i].g, original[i].b));
      assert(compositor.scene[20 * WC_WIDTH + 9] == rgb(126, 98, 70));
    }
  }
  leaf.value.ink = 0;
  const unsigned blends[] = {0, 15, 100};
  const uint32_t expected[] = {0xff46627e, 0xff42656c, 0xff31770a};
  for (unsigned i = 0; i < 3; i++) {
    leaf.value.blend = blends[i];
    render();
    assert(compositor.scene[22 * WC_WIDTH + 12] == expected[i]);
  }
  leaf.value.blend = 100;
  leaf.value.ink = 9;
  render();
  // Gray128 mask has127 coverage; direct Mask9 mixes on a256-step scale.
  unsigned sr = (17 * 127 + 210 * 129) >> 8, sg = (199 * 127 + 164 * 129) >> 8,
           sb = (83 * 127 + 117 * 129) >> 8;
  assert(compositor.scene[22 * WC_WIDTH + 12] ==
         rgb(sr * 154 >> 8, sg * 154 >> 8, sb * 154 >> 8));
  director.sprites[2].visible = false;
  dg_sprite_changed(&director, 2);
  leaf.value.ink = 39;
  place(4);
  dg_sprite_t *outer = &director.sprites[1];
  outer->value.x = outer->value.y = 0;
  outer->custom_quad = true;
  const float quad[] = {10, 10, 30, 12, 27, 34, 8, 29};
  memcpy(outer->quad, quad, sizeof(quad));
  inner.value.rotation = 2700;
  inner.value.skew = 1300;
  dg_sprite_t child = {.value = inner.value, .visible = true, .stretch = true};
  float inner_quad[8];
  dg_sprite_quad(&director, &child, inner_quad);
  uint32_t plane_builds = compositor.plane_builds;
  for (unsigned operation = 0; operation < 2; operation++) {
    // Copy is color-independent, but transforming its intermediate plane is
    // still not equivalent to sampling the transformed child's original.
    leaf.value.ink = operation ? 0 : 39;
    for (unsigned flips = 0; flips < 4; flips++) {
      outer->flip_h = flips & 1;
      outer->flip_v = flips & 2;
      render();
      for (unsigned y = 0; y < 50; y++)
        for (unsigned x = 0; x < 50; x++) {
          double u, v, s, t;
          bool covered = inverse(quad, x + .5, y + .5, &u, &v);
          if (flips & 1)
            u = 1 - u;
          if (flips & 2)
            v = 1 - v;
          covered = covered && inverse(inner_quad, u * 8, v * 8, &s, &t);
          assert(compositor.scene[y * WC_WIDTH + x] ==
                 (covered ? rgb(17, operation ? 199 : 164, 83)
                          : rgb(210, 164, 117)));
        }
    }
  }
  assert(compositor.plane_builds == plane_builds);
  // The same cached child metadata must use a newly changed backdrop.
  director.stage_color = 0x283c50;
  render();
  assert(compositor.scene[0] == rgb(40, 60, 80));
  wc_release(&compositor);
  dg_films_dispose(&director);
  dg_images_dispose(&director);
  puts("native film backdrop: original ink/blend/mask values, nested nonaffine "
       "clipping and flips passed");
}
