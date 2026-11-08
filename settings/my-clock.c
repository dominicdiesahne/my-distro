/*
 * my-clock: the Clock app of my-distro. A big analog clock face or a big
 * digital time, in the colors of the current theme. The switch at the bottom
 * changes the style; it is saved as "clock_app" in the settings file.
 *
 * Written with GTK 3; everything inside the window is drawn with cairo.
 * Once a second it redraws, and re-reads the settings if they changed.
 */
#include <gtk/gtk.h>

#include "i18n.h"
#include <math.h>
#include <string.h>

#include "dial.h"
#include "settings.h"

#define SWITCH_W 180
#define SWITCH_H 30

static struct settings cfg;
static time_t cfg_time;
static GtkWidget *area;

/* Where the Analog | Digital switch is, for clicks */
static double sw_x, sw_y;

static void load(void)
{
	settings_load(&cfg, NULL);
	cfg_time = settings_mtime(NULL);
}

/* Text centred on x, with its middle at y */
static void centred(cairo_t *cr, const char *s, double x, double y)
{
	cairo_text_extents_t e;

	cairo_text_extents(cr, s, &e);
	cairo_move_to(cr, x - e.width / 2 - e.x_bearing, y - e.height / 2 - e.y_bearing);
	cairo_show_text(cr, s);
}

static void draw_digital(cairo_t *cr, double w, double h, const struct tm *tm)
{
	char t[32], secs[8], date[64];
	struct settings c = cfg;
	double size = w / 5.2, y = h * 0.42;
	cairo_text_extents_t e;

	c.clock_seconds = 0;                   /* seconds drawn separately */
	settings_format_time(&c, tm, t, sizeof t);
	snprintf(secs, sizeof secs, "%02d", tm->tm_sec);
	strftime(date, sizeof date, "%A, %d %B %Y", tm);

	if (size > h / 3)
		size = h / 3;
	cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
	dial_color(cr, cfg.pal.text, 1);
	centred(cr, t, w / 2, y);

	/* seconds: a thin bar under the time that fills up once a minute */
	cairo_text_extents(cr, t, &e);
	{
		double bx = w / 2 - e.width / 2, bw = e.width, by = y + size * 0.62;

		cairo_rectangle(cr, bx, by, bw, 3);
		dial_color(cr, cfg.pal.button, 1);
		cairo_fill(cr);
		cairo_rectangle(cr, bx, by, bw * (tm->tm_sec + 1) / 60.0, 3);
		dial_color(cr, cfg.pal.accent, 1);
		cairo_fill(cr);
		if (cfg.clock_seconds) {
			cairo_set_font_size(cr, size * 0.3);
			dial_color(cr, cfg.pal.accent, 1);
			cairo_move_to(cr, bx + bw + 8, y + size * 0.36);
			cairo_show_text(cr, secs);
		}
	}

	cairo_set_font_size(cr, size * 0.22 < 13 ? 13 : size * 0.22);
	dial_color(cr, cfg.pal.dim, 1);
	centred(cr, date, w / 2, y + size * 0.62 + 34);
}

static void draw_analog(cairo_t *cr, double w, double h, const struct tm *tm)
{
	char date[64];
	double r = fmin(w, h - SWITCH_H - 70) / 2 - 16;

	dial_draw(cr, &cfg.pal, w / 2, r + 20, r, tm, 1);

	strftime(date, sizeof date, "%A, %d %B", tm);
	cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL,
	                       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	dial_color(cr, cfg.pal.dim, 1);
	centred(cr, date, w / 2, 2 * r + 46);
}

/* The "Analog | Digital" switch at the bottom */
static void draw_switch(cairo_t *cr, double w, double h)
{
	const char *names[] = { _("Digital"), _("Analog") };
	int on = cfg.clock_app;               /* CLOCK_DIGITAL = 0, CLOCK_ANALOG = 1 */

	sw_x = w / 2 - SWITCH_W / 2;
	sw_y = h - SWITCH_H - 14;
	cairo_set_font_size(cr, 13);
	for (int i = 0; i < 2; i++) {
		double x = sw_x + i * SWITCH_W / 2;

		cairo_rectangle(cr, x, sw_y, SWITCH_W / 2, SWITCH_H);
		dial_color(cr, i == on ? cfg.pal.accent : cfg.pal.button, 1);
		cairo_fill(cr);
		dial_color(cr, i == on ? cfg.pal.on_accent : cfg.pal.text, 1);
		centred(cr, names[i], x + SWITCH_W / 4, sw_y + SWITCH_H / 2);
	}
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
	double width = gtk_widget_get_allocated_width(w);
	double height = gtk_widget_get_allocated_height(w);
	time_t now = time(NULL);
	struct tm tm;

	(void)data;
	tzset();                 /* the time zone may have changed */
	localtime_r(&now, &tm);
	dial_color(cr, cfg.pal.bg, 1);
	cairo_paint(cr);
	if (cfg.clock_app == CLOCK_ANALOG)
		draw_analog(cr, width, height, &tm);
	else
		draw_digital(cr, width, height - SWITCH_H - 20, &tm);
	draw_switch(cr, width, height);
	return TRUE;
}

static gboolean on_click(GtkWidget *w, GdkEventButton *e, gpointer data)
{
	(void)w; (void)data;
	if (e->y < sw_y || e->y > sw_y + SWITCH_H || e->x < sw_x || e->x > sw_x + SWITCH_W)
		return FALSE;
	load();                               /* the newest settings, then change one */
	cfg.clock_app = e->x < sw_x + SWITCH_W / 2 ? CLOCK_DIGITAL : CLOCK_ANALOG;
	settings_save(&cfg);
	cfg_time = settings_mtime(NULL);
	gtk_widget_queue_draw(area);
	return TRUE;
}

static gboolean tick(gpointer data)
{
	(void)data;
	if (settings_mtime(NULL) != cfg_time)
		load();
	gtk_widget_queue_draw(area);
	return G_SOURCE_CONTINUE;
}

/* The window icon (taskbar, Alt+Tab): the clock face, drawn at 64x64 */
static GdkPixbuf *make_icon(void)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
	cairo_t *cr = cairo_create(s);
	struct tm tm = { .tm_hour = 10, .tm_min = 10 };
	GdkPixbuf *pb;

	dial_draw(cr, &cfg.pal, 32, 32, 30, &tm, 0);
	cairo_destroy(cr);
	pb = gdk_pixbuf_get_from_surface(s, 0, 0, 64, 64);
	cairo_surface_destroy(s);
	return pb;
}

int main(int argc, char **argv)
{
	GtkWidget *win;
	GdkPixbuf *icon;

	i18n_init();
	gtk_init(&argc, &argv);
	load();

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), _("Clock"));
	gtk_window_set_default_size(GTK_WINDOW(win), 340, 400);
	icon = make_icon();
	gtk_window_set_icon(GTK_WINDOW(win), icon);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	area = gtk_drawing_area_new();
	gtk_widget_set_size_request(area, 240, 280);
	gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(area, "draw", G_CALLBACK(on_draw), NULL);
	g_signal_connect(area, "button-press-event", G_CALLBACK(on_click), NULL);
	gtk_container_add(GTK_CONTAINER(win), area);

	g_timeout_add(1000, tick, NULL);
	gtk_widget_show_all(win);
	gtk_main();
	return 0;
}
