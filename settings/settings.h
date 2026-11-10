/*
 * settings.h - the user's look-and-feel settings, shared by the taskbar
 * (my-panel), the login screen, the clock and the Settings app.
 *
 * They are plain "key = value" lines. First /etc/my-distro/settings is read
 * (defaults for everyone), then ~/.config/my-distro/settings (your own).
 * The Settings app writes the second file; you can also edit it by hand.
 */
#ifndef MY_SETTINGS_H
#define MY_SETTINGS_H

#include <time.h>

#define SYSTEM_SETTINGS "/etc/my-distro/settings"
#define USER_SETTINGS   ".config/my-distro/settings"   /* in the home folder */

/* The colors of one theme, as "#rrggbb" */
struct palette {
	const char *name;      /* "teal", "ocean", ... */
	const char *label;     /* "Teal" (shown in the Settings app) */
	int dark;              /* 1: light text on dark background */
	char bg[8];            /* taskbar, menus, window titles */
	char button[8];        /* buttons on the taskbar */
	char hover[8];         /* button under the mouse */
	char active[8];        /* pressed / focused button */
	char text[8];
	char dim[8];           /* secondary text */
	char accent[8];        /* highlights, the theme's main color */
	char sep[8];           /* lines between things */
	char on_accent[8];     /* text on an accent-colored button */
};

enum { CLOCK_DIGITAL, CLOCK_ANALOG };
enum { PANEL_BOTTOM, PANEL_TOP };

struct settings {
	char theme[16];        /* name of a palette */
	char accent[8];        /* "" = the theme's own accent color */
	struct palette pal;    /* the theme's colors, accent already applied */

	char wallpaper[256];   /* "theme" = the theme's wallpaper, "color" =
	                          one color (pal.bg), else a picture's path */
	char wallpaper_mode[32];/* crop, fit, center, tile, stretch */

	int panel_clock;       /* CLOCK_DIGITAL or CLOCK_ANALOG */
	int clock_app;         /* style of the Clock app window */
	int clock_24h;         /* 1: 14:32, 0: 2:32 PM */
	int clock_seconds;
	int clock_date;        /* show the date next to the time */
	char date_format[32];  /* strftime format of that date */

	int panel_position;    /* PANEL_BOTTOM or PANEL_TOP */
	int panel_height;      /* pixels, 28-56 */
	int font_size;         /* points, 8-16 */

	int lock_minutes;      /* lock the screen after this long without use; 0 = never */
	int scale;             /* text and icons in percent: 100-200 (Xft.dpi = 96 * scale) */
	int cursor_size;       /* mouse pointer in pixels: 24 (normal) - 96 */
	int screen_reader;     /* 1: my-reader starts at login (0.9.7) */

	/* Read by /usr/lib/my-distro/session-env at login, for the session: */
	char language[32];     /* LANG, e.g. "de_DE.UTF-8"; "" = the system's */
	char proxy[160];       /* "http://host:port" for web and updates; "" = none */
	char no_proxy[160];    /* addresses reached without the proxy */
};

/* High-contrast themes ("contrast", "contrast-light"): plain colors, no
 * picture behind the desktop, and GTK's own high-contrast style. */
int settings_high_contrast(const struct settings *s);

/* All built-in themes, ending with { NULL } */
extern const struct palette themes[];

/* Date formats offered by the Settings app, ending with NULL */
extern const char *date_formats[];

/* Fills s with the defaults, then the system file, then the user's file in
 * the home folder `home` (NULL = $HOME). */
void settings_load(struct settings *s, const char *home);

/* Writes the user's file (makes ~/.config/my-distro if needed). 0 = ok */
int settings_save(const struct settings *s);

/* Full path of the user's settings file in buf. */
void settings_user_path(char *buf, int len, const char *home);

/* Picture file for the wallpaper setting ("" for one color). */
void settings_wallpaper_file(const struct settings *s, char *buf, int len);

/* Time and date as text, the way the settings say. */
void settings_format_time(const struct settings *s, const struct tm *tm,
                          char *buf, int len);
void settings_format_date(const struct settings *s, const struct tm *tm,
                          char *buf, int len);

/* Modification time of the user's file, to notice changes (0 = none). */
time_t settings_mtime(const char *home);

#endif
