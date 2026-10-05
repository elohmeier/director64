#include "shape.h"
#include <math.h>

static int inset(unsigned kind, int w, int h, int y) {
  if (kind == 1)
    return 0;
  double rx = w * 0.5, ry = h * 0.5;
  if (kind == 2) {
    if (rx > 12)
      rx = 12;
    if (ry > 12)
      ry = 12;
    if (y >= ry && y < h - ry)
      return 0;
  }
  double dy = y < h / 2 ? ry - y - 0.5 : y + 0.5 - (h - ry);
  double norm = dy / ry;
  double square = 1 - norm * norm;
  return (int)ceil(rx * (1 - sqrt(square > 0 ? square : 0)) - 0.5);
}
unsigned dg_shape_spans(unsigned kind, int w, int h, int y, unsigned line,
                        bool filled, bool reverse, int out[4]) {
  if (w <= 0 || h <= 0 || y < 0 || y >= h || kind < 1 || kind > 4)
    return 0;
  if (kind == 4) {
    if (!line)
      return 0;
    int a = (int)((long long)y * w / h);
    int b = (int)((long long)(y + 1) * w / h) + 1;
    a -= (int)line / 2;
    b += (int)(line - 1) / 2;
    if (a < 0)
      a = 0;
    if (b > w)
      b = w;
    out[0] = reverse ? w - b : a;
    out[1] = reverse ? w - a : b;
    return 1;
  }
  if (!filled && !line)
    return 0;
  int outer = inset(kind, w, h, y);
  if (outer < 0)
    outer = 0;
  out[0] = outer;
  out[1] = w - outer;
  if (filled || line * 2 >= (unsigned)w || line * 2 >= (unsigned)h ||
      y < (int)line || y >= h - (int)line)
    return 1;
  int inner = (int)line + inset(kind, w - line * 2, h - line * 2, y - line);
  if (inner < outer)
    inner = outer;
  out[1] = inner;
  out[2] = w - inner;
  out[3] = w - outer;
  return 2;
}
