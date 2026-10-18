#ifndef DIRECTOR64_SHAPE_H
#define DIRECTOR64_SHAPE_H
#include <stdbool.h>
// Monochrome Director pattern (1..56), anchored in authored stage pixels.
// Zero is the historical solid-fill default for descriptors without a pattern.
bool dg_shape_pattern(unsigned pattern, int x, int y);
// Up to two half-open horizontal spans for a clipped row of a Director shape.
unsigned dg_shape_spans(unsigned kind, int width, int height, int y,
                        unsigned line, bool filled, bool reverse, int out[4]);
bool dg_shape_contains(unsigned kind, int width, int height, int x, int y,
                        unsigned line, bool filled, bool reverse);
#endif
