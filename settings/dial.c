/*
 * dial.c - the analog clock face (see dial.h).
 *
 * Angles: 0 is 12 o'clock and they grow clockwise, so a point at angle a and
 * distance d from the centre is (cx + sin(a) * d, cy - cos(a) * d).
 */
#include "dial.h"

#include <math.h>
#include <stdio.h>

void dial_color(cairo_t *cr, const char *hex, double alpha)
{
	unsigned r = 0, g = 0, b = 0;

	sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b);
	cairo_set_source_rgba(cr, r / 255.0, g / 255.0, b / 255.0, alpha);
}

/* A line from distance d1 to d2 at angle a */
static void ray(cairo_t *cr, double cx, double cy, double a, double d1, double d2)
{
	cairo_move_to(cr, cx + sin(a) * d1, cy - cos(a) * d1);
	cairo_line_to(cr, cx + sin(a) * d2, cy - cos(a) * d2);
	cairo_stroke(cr);
}

void dial_draw(cairo_t *cr, const struct palette *pal, double cx, double cy,
               double r, const struct tm *tm, int seconds)
{
	int large = r > 40;           /* the Clock app: draw minute marks too */
	double a;

	cairo_save(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

	/* face, and a ring in the accent color */
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	dial_color(cr, pal->button, 1);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, large ? r * 0.035 : 1.5);
	dial_color(cr, pal->accent, 1);
	cairo_stroke(cr);

	/* marks: 60 small ones (large dial only), 12 hours, 4 quarters longer */
	for (int i = 0; i < 60; i++) {
		int hour = i % 5 == 0, quarter = i % 15 == 0;

		if (!hour && !large)
			continue;
		a = i * M_PI / 30;
		cairo_set_line_width(cr, quarter ? r * 0.055 : hour ? r * 0.035 : r * 0.012);
		dial_color(cr, quarter ? pal->text : pal->dim, hour ? 1 : 0.6);
		ray(cr, cx, cy, a, r * (quarter ? 0.68 : hour ? 0.75 : 0.82), r * 0.86);
	}

	/* hour hand */
	a = (tm->tm_hour % 12 + tm->tm_min / 60.0) * M_PI / 6;
	cairo_set_line_width(cr, r * 0.09);
	dial_color(cr, pal->text, 1);
	ray(cr, cx, cy, a, 0, r * 0.5);

	/* minute hand */
	a = (tm->tm_min + tm->tm_sec / 60.0) * M_PI / 30;
	cairo_set_line_width(cr, r * 0.06);
	ray(cr, cx, cy, a, 0, r * 0.76);

	/* second hand, with a short tail */
	if (seconds) {
		a = tm->tm_sec * M_PI / 30;
		cairo_set_line_width(cr, large ? r * 0.02 : 1);
		dial_color(cr, pal->accent, 1);
		ray(cr, cx, cy, a, -r * 0.15, r * 0.82);
	}

	/* centre */
	cairo_arc(cr, cx, cy, r * (large ? 0.05 : 0.09), 0, 2 * M_PI);
	dial_color(cr, pal->accent, 1);
	cairo_fill(cr);
	cairo_restore(cr);
}
