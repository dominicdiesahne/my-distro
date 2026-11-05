/*
 * dial.h - draws the analog clock face of my-distro with cairo. Used by the
 * taskbar (small) and the Clock app (big), so both look the same.
 */
#ifndef MY_DIAL_H
#define MY_DIAL_H

#include <cairo.h>
#include <time.h>

#include "settings.h"

/* "#rrggbb" -> cairo source color */
void dial_color(cairo_t *cr, const char *hex, double alpha);

/* Clock face centred on (cx, cy) with radius r, showing the time in tm. */
void dial_draw(cairo_t *cr, const struct palette *pal, double cx, double cy,
               double r, const struct tm *tm, int seconds);

#endif
