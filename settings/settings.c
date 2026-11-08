/*
 * settings.c - reads and writes the look-and-feel settings (see settings.h).
 */
#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*                name      label     dark bg         button     hover      active     text       dim        accent     sep        on_accent */
const struct palette themes[] = {
	{ "teal",   "Teal",   1, "#0b2a2b", "#134e4f", "#1d6466", "#2c7a7b", "#d8f3f0", "#7fa9a6", "#4fd1c5", "#2c5a5b", "#062022" },
	{ "ocean",  "Ocean",  1, "#0b1a2e", "#14304f", "#1d4466", "#2c5a85", "#dce9f8", "#8aa3c0", "#5aa9ff", "#2c4460", "#04121f" },
	{ "violet", "Violet", 1, "#1a1230", "#2d2152", "#3d2e6b", "#503d8a", "#ece6fb", "#a497c6", "#b48cff", "#3b3160", "#160b2b" },
	{ "ember",  "Ember",  1, "#26110e", "#4a1f1a", "#622a22", "#7d3a2e", "#fbe7e2", "#c49a90", "#ff7a59", "#5a2e28", "#2a0c06" },
	{ "forest", "Forest", 1, "#0f2417", "#1b3d29", "#255236", "#316a46", "#e1f4e7", "#8fb79c", "#6ee7a0", "#2a4a35", "#06200f" },
	{ "light",  "Light",  0, "#eef3f3", "#dce7e7", "#cddede", "#b9d2d1", "#12302f", "#5c7a78", "#138a80", "#c3d3d2", "#ffffff" },
	/* High contrast (Accessibility): pure black and white, yellow / blue */
	{ "contrast",       "High contrast",       1, "#000000", "#000000", "#333333", "#4d4d00", "#ffffff", "#ffff00", "#ffff00", "#ffffff", "#000000" },
	{ "contrast-light", "High contrast light", 0, "#ffffff", "#ffffff", "#dddddd", "#c8d8ff", "#000000", "#000000", "#0000c0", "#000000", "#ffffff" },
	{ NULL }
};

int settings_high_contrast(const struct settings *s)
{
	return strncmp(s->theme, "contrast", 8) == 0;
}

const char *date_formats[] = {
	"%a %d.%m.",          /* Wed 04.11. */
	"%d.%m.%Y",           /* 04.11.2026 */
	"%a, %d %b",          /* Wed, 04 Nov */
	"%A, %d %B",          /* Wednesday, 04 November */
	"%Y-%m-%d",           /* 2026-11-04 */
	NULL
};

static void defaults(struct settings *s)
{
	memset(s, 0, sizeof *s);
	strcpy(s->theme, "teal");
	strcpy(s->wallpaper, "theme");
	strcpy(s->wallpaper_mode, "crop");
	s->panel_clock = CLOCK_DIGITAL;
	s->clock_app = CLOCK_ANALOG;
	s->clock_24h = 1;
	s->clock_seconds = 0;
	s->clock_date = 1;
	strcpy(s->date_format, date_formats[0]);
	s->panel_position = PANEL_BOTTOM;
	s->panel_height = 36;
	s->font_size = 10;
	s->lock_minutes = 10;
	s->scale = 100;
	s->cursor_size = 24;
}

static int yes(const char *v)
{
	return strcmp(v, "yes") == 0 || strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
}

static int is_color(const char *v)
{
	if (strlen(v) != 7 || v[0] != '#')
		return 0;
	for (int i = 1; i < 7; i++)
		if (!strchr("0123456789abcdefABCDEF", v[i]))
			return 0;
	return 1;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* Copies src into a fixed-size field, cut off if too long. */
#define SET(field, src) snprintf(field, sizeof field, "%s", src)

static void apply(struct settings *s, const char *key, const char *v)
{
	if (strcmp(key, "theme") == 0)
		SET(s->theme, v);
	else if (strcmp(key, "accent") == 0)
		SET(s->accent, is_color(v) ? v : "");
	else if (strcmp(key, "wallpaper") == 0)
		SET(s->wallpaper, v);
	else if (strcmp(key, "wallpaper_mode") == 0)
		SET(s->wallpaper_mode, v);
	else if (strcmp(key, "panel_clock") == 0)
		s->panel_clock = strcmp(v, "analog") == 0 ? CLOCK_ANALOG : CLOCK_DIGITAL;
	else if (strcmp(key, "clock_app") == 0)
		s->clock_app = strcmp(v, "digital") == 0 ? CLOCK_DIGITAL : CLOCK_ANALOG;
	else if (strcmp(key, "clock_24h") == 0)
		s->clock_24h = yes(v);
	else if (strcmp(key, "clock_seconds") == 0)
		s->clock_seconds = yes(v);
	else if (strcmp(key, "clock_date") == 0)
		s->clock_date = yes(v);
	else if (strcmp(key, "date_format") == 0)
		SET(s->date_format, v);
	else if (strcmp(key, "panel_position") == 0)
		s->panel_position = strcmp(v, "top") == 0 ? PANEL_TOP : PANEL_BOTTOM;
	else if (strcmp(key, "panel_height") == 0)
		s->panel_height = clamp(atoi(v), 28, 56);
	else if (strcmp(key, "font_size") == 0)
		s->font_size = clamp(atoi(v), 8, 16);
	else if (strcmp(key, "lock_minutes") == 0)
		s->lock_minutes = clamp(atoi(v), 0, 240);
	else if (strcmp(key, "scale") == 0)
		s->scale = clamp(atoi(v), 100, 200);
	else if (strcmp(key, "cursor_size") == 0)
		s->cursor_size = clamp(atoi(v), 24, 96);
}

/* Reads one file of "key = value" lines; '#' starts a comment line. */
static void read_file(struct settings *s, const char *path)
{
	FILE *f = fopen(path, "r");
	char line[400];

	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *key = line, *eq, *v, *end;

		line[strcspn(line, "\r\n")] = '\0';
		while (*key == ' ' || *key == '\t')
			key++;
		eq = strchr(key, '=');
		if (*key == '#' || !eq)
			continue;
		/* trim spaces around key and value */
		for (end = eq; end > key && (end[-1] == ' ' || end[-1] == '\t'); end--)
			;
		*end = '\0';
		for (v = eq + 1; *v == ' ' || *v == '\t'; v++)
			;
		for (end = v + strlen(v); end > v && (end[-1] == ' ' || end[-1] == '\t'); end--)
			;
		*end = '\0';
		apply(s, key, v);
	}
	fclose(f);
}

void settings_user_path(char *buf, int len, const char *home)
{
	if (!home)
		home = getenv("HOME");
	snprintf(buf, len, "%s/%s", home ? home : "/root", USER_SETTINGS);
}

void settings_load(struct settings *s, const char *home)
{
	char path[512];
	int i;

	defaults(s);
	read_file(s, SYSTEM_SETTINGS);
	settings_user_path(path, sizeof path, home);
	read_file(s, path);

	for (i = 0; themes[i].name; i++)
		if (strcmp(themes[i].name, s->theme) == 0)
			break;
	if (!themes[i].name) {          /* unknown theme: use the first one */
		i = 0;
		SET(s->theme, themes[0].name);
	}
	s->pal = themes[i];
	if (s->accent[0])
		SET(s->pal.accent, s->accent);
}

time_t settings_mtime(const char *home)
{
	char path[512];
	struct stat st;

	settings_user_path(path, sizeof path, home);
	return stat(path, &st) == 0 ? st.st_mtime : 0;
}

int settings_save(const struct settings *s)
{
	char path[512], dir[512], tmp[520];
	FILE *f;

	settings_user_path(path, sizeof path, NULL);
	snprintf(dir, sizeof dir, "%s", path);
	*strrchr(dir, '/') = '\0';                 /* ~/.config/my-distro */
	*strrchr(dir, '/') = '\0';                 /* ~/.config */
	mkdir(dir, 0755);
	snprintf(dir, sizeof dir, "%s", path);
	*strrchr(dir, '/') = '\0';
	if (mkdir(dir, 0755) < 0 && errno != EEXIST)
		return -1;

	/* Write a new file and rename it over the old one, so programs that
	 * read the file at the same moment never see half of it. */
	snprintf(tmp, sizeof tmp, "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f,
		"# my-distro settings. Changed by the Settings app; you can edit it too.\n"
		"# The taskbar and the clock notice changes within a second.\n\n"
		"# Colors: teal, ocean, violet, ember, forest or light.\n"
		"# accent = #rrggbb replaces the theme's highlight color (empty = theme's).\n"
		"theme = %s\naccent = %s\n\n"
		"# wallpaper = theme (matches the colors), color (plain), or a picture's path\n"
		"# wallpaper_mode = crop, fit, center, tile or stretch\n"
		"wallpaper = %s\nwallpaper_mode = %s\n\n"
		"# Clock: digital or analog, in the taskbar and in the Clock app\n"
		"panel_clock = %s\nclock_app = %s\n"
		"clock_24h = %s\nclock_seconds = %s\nclock_date = %s\n"
		"# date_format: see `man strftime`, e.g. %%a %%d.%%m. = Wed 04.11.\n"
		"date_format = %s\n\n"
		"# Taskbar: bottom or top; height 28-56 pixels; font size 8-16\n"
		"panel_position = %s\npanel_height = %d\nfont_size = %d\n\n"
		"# Lock the screen after this many minutes without use (0 = never)\n"
		"lock_minutes = %d\n"
		"# Size of text and icons in percent, 100-200 (at the next login)\n"
		"scale = %d\n"
		"# Size of the mouse pointer in pixels: 24 (normal), 32, 48, 64, 96\n"
		"cursor_size = %d\n",
		s->theme, s->accent, s->wallpaper, s->wallpaper_mode,
		s->panel_clock == CLOCK_ANALOG ? "analog" : "digital",
		s->clock_app == CLOCK_ANALOG ? "analog" : "digital",
		s->clock_24h ? "yes" : "no", s->clock_seconds ? "yes" : "no",
		s->clock_date ? "yes" : "no", s->date_format,
		s->panel_position == PANEL_TOP ? "top" : "bottom",
		s->panel_height, s->font_size, s->lock_minutes, s->scale, s->cursor_size);
	if (fclose(f) != 0)
		return -1;
	return rename(tmp, path);
}

void settings_wallpaper_file(const struct settings *s, char *buf, int len)
{
	if (strcmp(s->wallpaper, "color") == 0 ||
	    (settings_high_contrast(s) && strcmp(s->wallpaper, "theme") == 0))
		buf[0] = '\0';                     /* one color (high contrast: plain) */
	else if (strcmp(s->wallpaper, "theme") == 0 || !s->wallpaper[0])
		snprintf(buf, len, "/usr/share/backgrounds/my-distro-%s.png", s->theme);
	else
		snprintf(buf, len, "%s", s->wallpaper);
}

void settings_format_time(const struct settings *s, const struct tm *tm,
                          char *buf, int len)
{
	const char *fmt = s->clock_24h ? (s->clock_seconds ? "%H:%M:%S" : "%H:%M")
	                               : (s->clock_seconds ? "%l:%M:%S %p" : "%l:%M %p");

	strftime(buf, len, fmt, tm);
	if (buf[0] == ' ')                          /* %l pads 1-9 with a space */
		memmove(buf, buf + 1, strlen(buf));
}

void settings_format_date(const struct settings *s, const struct tm *tm,
                          char *buf, int len)
{
	if (strftime(buf, len, s->date_format, tm) == 0)
		buf[0] = '\0';
}
