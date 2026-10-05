#ifndef DIRECTOR64_SHAPE_H
#define DIRECTOR64_SHAPE_H
#include <stdbool.h>
// Up to two half-open horizontal spans for a clipped row of a Director shape.
unsigned dg_shape_spans(unsigned kind, int width, int height, int y,
                        unsigned line, bool filled, bool reverse, int out[4]);
#endif
