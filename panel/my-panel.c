/*
 * my-panel: the taskbar of my-distro, written with Xlib, Xft (text) and
 * Imlib2 (icons).
 *
 *   [ â˜° Start ] [ window ] [ window ] ...                    [ Mon 04.11.  14:32 ]
 *
 * - Start button opens the app menu. The entries come from /etc/my-panel/menu,
 *   one "Label|command|icon" per line, "-" for a separator line. Programs
 *   from the Software app (my-pkg writes /var/lib/my-pkg/menu, same format)
 *   go after the first group; the menu is read again when that file changes.
 *   `killall -USR1 my-panel` opens it too (Openbox binds that to Super+Space).
 *   Pressing and releasing the Windows key (Super) on its own opens and
 *   closes it as well: XInput 2 reports every key to us, even while Openbox
 *   holds the keyboard, so Super+L and the other shortcuts still work (another
 *   key between press and release means it was a shortcut, not the menu).
 *   When the menu is taller than the screen it gets more columns.
 * - One button per open window, with the app's icon. Click: bring it to the
 *   front; click the window that is already in front: minimise it. The x on
 *   the button (shown under the mouse and on the active window) closes it.
 *   The icon comes from the window itself (_NET_WM_ICON, GTK apps set it) or,
 *   for apps that don't have one, from /etc/my-panel/icons (window class ->
 *   icon name in the Adwaita icon theme).
 * - Clock on the right: time and date, or a small analog clock. Click it
 *   for a calendar.
 * - Wi-Fi bars (signal strength from /proc/net/wireless) when the computer
 *   has a Wi-Fi adapter; a click opens the Wi-Fi window (my-wifi).
 * - "Update to 0.9" next to the clock when a new version of my-distro is out.
 *   my-update watch writes that into ~/.cache/my-distro/update; a click
 *   opens System update (my-setup --update).
 * - A speaker with the volume, when there is a sound card (ALSA's mixer).
 *   Scroll on it: louder / quieter; click: sound off / on; right-click: all
 *   controls (alsamixer). The volume keys (my-volume) change the same
 *   control, and the speaker follows. Your volume is saved in
 *   ~/.config/my-distro/volume and set again at the next login.
 * - A battery with its charge, on laptops (/sys/class/power_supply). Below
 *   10 % while not charging it warns once with a notification.
 *
 * Colors, size, position and the clock's style come from the settings file
 * (settings/settings.h). When it changes, the panel restarts itself.
 *
 * The window manager (Openbox) tells us about windows through properties on
 * the root window (the EWMH standard): _NET_CLIENT_LIST lists all windows,
 * _NET_ACTIVE_WINDOW names the focused one. We watch those properties and
 * redraw when they change. We ask Openbox to keep our strip of the screen
 * free with _NET_WM_STRUT, so maximised windows stop above the panel.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/XInput2.h>
#include <Imlib2.h>
#include <alsa/asoundlib.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <dirent.h>
#include <math.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dial.h"
#include "settings.h"

#define PAD         4
#define START_W     100
#define TASK_MAX_W  220
#define MENU_W      240
#define HEADER_H    40
#define ITEM_H      30
#define SEP_H       11
#define MAX_TASKS   64
#define MAX_ITEMS   128
#define TITLE_MAX   200
#define ICON_SIZE   20
#define MAX_CACHE   160
#define CLOSE_W     22       /* the x on a window button */
#define CAL_W       296      /* calendar popup */
#define CAL_TOP     92       /* height of the big time + date at its top */
#define CAL_CELL    38

#define MENU_FILE  "/etc/my-panel/menu"
#define PKG_MENU   "/var/lib/my-pkg/menu"    /* programs from the Software app */
#define ICONS_FILE "/etc/my-panel/icons"

enum { HOVER_BATTERY = -7, HOVER_VOLUME = -6, HOVER_WIFI = -5, HOVER_UPDATE = -4,
       HOVER_CLOCK = -3, HOVER_NONE = -2, HOVER_START = -1 };

#define UPDATE_FILE ".cache/my-distro/update"   /* in the home folder */
#define VOLUME_FILE ".config/my-distro/volume"  /* in the home folder */
#define VOLUME_STEP 5                            /* percent per scroll step */
#define BATTERY_LOW 10                           /* percent: warn once */

struct task {
	Window win;
	char title[TITLE_MAX + 1];
	int hidden;
	int x, w;              /* position of its button on the panel */
	Imlib_Image icon;
	int own_icon;          /* 1: made from _NET_WM_ICON, we must free it */
};

struct item {
	char label[64];
	char cmd[256];         /* empty for a separator line */
	int x, y, h;           /* position inside the menu window (x: its column) */
	Imlib_Image icon;
};

static Display *dpy;
static int scr;
static Window root, panel, menu, cal;
static Pixmap buf, menu_buf, cal_buf;
static XftDraw *draw;
static Visual *vis;
static Colormap cmap;
static XftFont *font, *bold, *small, *big;
static int sw, sh;          /* screen size */

static struct settings cfg;
static int panel_h, panel_y;  /* height and top edge of the panel */
static char **self_argv;     /* to restart ourselves when settings change */

static XftColor c_bg, c_button, c_hover, c_active, c_text, c_dim, c_accent, c_sep,
	c_on_accent;

static struct task tasks[MAX_TASKS];
static int ntasks;
static Window active;
static int hover = HOVER_NONE;
static int hover_close;      /* the mouse is on the x of button `hover` */

static struct item items[MAX_ITEMS];
static int nitems, menu_w = MENU_W, menu_h, menu_open, menu_hover = -1, item_h = ITEM_H;
static XftDraw *menu_draw;

/* The Windows key alone (XInput 2 raw key events) */
static int xi_opcode = -1;        /* -1: the X server has no XInput 2 */
static KeyCode super_l, super_r;
static int super_armed;           /* Super is down and nothing else was pressed */
static Atom A_LOCK;               /* owned by the screen lock while it is on */

static char clock_text[64], date_text[64];
static int clock_x, clock_w;  /* clock area on the panel */
static char update_version[32];   /* "" = no update; else the new version */
static int update_x, update_w;    /* its button (update_w = 0: not shown) */
static time_t update_mtime;
static int wifi_level = -2;       /* -2 no Wi-Fi adapter, -1 not connected, 1-4 bars */
static int wifi_x, wifi_w;        /* its button (wifi_w = 0: not shown) */
static time_t clock_now;

/* Sound: the mixer of the default sound card and the control we show */
static snd_mixer_t *mixer;
static snd_mixer_elem_t *vol_elem;
static int vol_pct = -1;          /* -1 = no sound card */
static int vol_on = 1;            /* 0 = muted */
static time_t vol_retry;          /* when we last tried to open the mixer */
static int vol_x, vol_w;

/* Battery */
static int bat_pct = -1;          /* -1 = no battery */
static int bat_charging, bat_warned;
static time_t bat_read;
static int bat_x, bat_w;

/* Calendar popup */
static XftDraw *cal_draw;
static int cal_h, cal_open, cal_year, cal_month;   /* month shown, 0-11 */
static int cal_hover = -1;    /* -1 none, 0 "<", 1 ">" */

/* Icons loaded from the theme, by name (NULL = not found, don't search again) */
static struct { char name[64]; Imlib_Image img; } icon_cache[MAX_CACHE];
static int nicon_cache;

/* Window class -> icon name, from /etc/my-panel/icons */
static struct { char cls[64]; char icon[64]; } class_icons[MAX_CACHE];
static int nclass_icons;
static volatile sig_atomic_t menu_requested;   /* set by SIGUSR1 */
static time_t pkg_menu_time;      /* when PKG_MENU was last changed (0: none) */

static Atom A_ICON, A_CLIENT_LIST, A_ACTIVE, A_NAME, A_UTF8, A_STATE, A_HIDDEN,
	A_SKIP_TASKBAR, A_TYPE, A_TYPE_DOCK, A_TYPE_DESKTOP, A_STRUT,
	A_STRUT_PARTIAL, A_STICKY, A_ABOVE, A_DESKTOP, A_CLOSE;

/* Windows disappear all the time; asking about a closed one is not an error. */
static int ignore_x_errors(Display *d, XErrorEvent *e)
{
	(void)d; (void)e;
	return 0;
}

static void color(XftColor *c, const char *name)
{
	if (!XftColorAllocName(dpy, vis, cmap, name, c)) {
		fprintf(stderr, "my-panel: can't allocate color %s\n", name);
		exit(1);
	}
}

/* Reads a window property. Returns malloc'd data (free with XFree) or NULL. */
static void *get_prop(Window w, Atom prop, Atom type, unsigned long *n)
{
	Atom real_type;
	int format;
	unsigned long count, after;
	unsigned char *data = NULL;

	/* up to 1M values: big enough for a 512x512 window icon */
	if (XGetWindowProperty(dpy, w, prop, 0, 1 << 20, False, type, &real_type,
	                       &format, &count, &after, &data) != Success || !data)
		return NULL;
	if (n)
		*n = count;
	return data;
}

static int has_atom(Window w, Atom prop, Atom want)
{
	unsigned long n = 0;
	Atom *list = get_prop(w, prop, XA_ATOM, &n);
	int found = 0;

	for (unsigned long i = 0; list && i < n; i++)
		if (list[i] == want)
			found = 1;
	if (list)
		XFree(list);
	return found;
}

static void window_title(Window w, char *out)
{
	char *name = get_prop(w, A_NAME, A_UTF8, NULL);

	if (!name)
		XFetchName(dpy, w, &name);
	snprintf(out, TITLE_MAX + 1, "%s", name && *name ? name : "Window");
	if (name)
		XFree(name);
}

/* Finds an icon by name in the icon theme, e.g. "utilities-terminal". */
static Imlib_Image theme_icon(const char *name)
{
	static const char *patterns[] = {
		"/usr/share/icons/Adwaita/24x24/*/%s.png",
		"/usr/share/icons/Adwaita/32x32/*/%s.png",
		"/usr/share/icons/Adwaita/48x48/*/%s.png",
		"/usr/share/icons/hicolor/48x48/apps/%s.png",
		"/usr/share/pixmaps/%s.png",
	};
	Imlib_Image img = NULL;
	char pat[256];

	if (!name || !*name)
		return NULL;
	for (int i = 0; i < nicon_cache; i++)
		if (strcmp(icon_cache[i].name, name) == 0)
			return icon_cache[i].img;

	if (name[0] == '/')
		img = imlib_load_image(name);
	for (size_t i = 0; !img && i < sizeof patterns / sizeof *patterns; i++) {
		glob_t g;

		snprintf(pat, sizeof pat, patterns[i], name);
		if (glob(pat, 0, NULL, &g) == 0)
			for (size_t j = 0; !img && j < g.gl_pathc; j++)
				img = imlib_load_image(g.gl_pathv[j]);
		globfree(&g);
	}
	if (nicon_cache < MAX_CACHE) {
		snprintf(icon_cache[nicon_cache].name, 64, "%s", name);
		icon_cache[nicon_cache++].img = img;
	}
	return img;
}

static void load_class_icons(void)
{
	FILE *f = fopen(ICONS_FILE, "r");
	char line[200], *eq;

	if (!f)
		return;
	while (fgets(line, sizeof line, f) && nclass_icons < MAX_CACHE) {
		line[strcspn(line, "\r\n")] = '\0';
		eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = '\0';
		snprintf(class_icons[nclass_icons].cls, 64, "%.63s", line);
		snprintf(class_icons[nclass_icons].icon, 64, "%.63s", eq + 1);
		nclass_icons++;
	}
	fclose(f);
}

/*
 * The window's own icon: _NET_WM_ICON holds one or more images, each as
 * width, height, then width*height pixels in ARGB. Pick the smallest one
 * that is at least ICON_SIZE wide (or else the biggest).
 */
static Imlib_Image net_wm_icon(Window w)
{
	unsigned long n = 0, i = 0, best = 0, bw = 0, bh = 0;
	unsigned long *d = get_prop(w, A_ICON, XA_CARDINAL, &n);
	Imlib_Image img = NULL;

	if (!d)
		return NULL;
	while (i + 2 <= n) {
		unsigned long iw = d[i], ih = d[i + 1];

		if (iw == 0 || ih == 0 || i + 2 + iw * ih > n)
			break;
		if (bw == 0 ||
		    (iw >= ICON_SIZE && (bw < ICON_SIZE || iw < bw)) ||
		    (iw < ICON_SIZE && bw < ICON_SIZE && iw > bw)) {
			best = i;
			bw = iw;
			bh = ih;
		}
		i += 2 + iw * ih;
	}
	if (bw > 0) {
		/* Xlib returns 32-bit values as longs (64 bits here): repack them */
		DATA32 *px = malloc(bw * bh * sizeof *px);

		if (px) {
			for (unsigned long k = 0; k < bw * bh; k++)
				px[k] = (DATA32)d[best + 2 + k];
			img = imlib_create_image_using_copied_data(bw, bh, px);
			free(px);
			if (img) {
				imlib_context_set_image(img);
				imlib_image_set_has_alpha(1);
			}
		}
	}
	XFree(d);
	return img;
}

/* Icon for an app without its own: look its window class up in the map. */
static Imlib_Image class_icon(Window w)
{
	XClassHint ch = { 0 };
	Imlib_Image img = NULL;

	if (XGetClassHint(dpy, w, &ch)) {
		/* The window's own name first, then its class: "xterm -name my-ide"
		 * has the name my-ide but the class XTerm, and must get my-ide's
		 * icon, not the one for all xterms. */
		for (int i = 0; !img && ch.res_name && i < nclass_icons; i++)
			if (strcasecmp(ch.res_name, class_icons[i].cls) == 0)
				img = theme_icon(class_icons[i].icon);
		for (int i = 0; !img && ch.res_class && i < nclass_icons; i++)
			if (strcasecmp(ch.res_class, class_icons[i].cls) == 0)
				img = theme_icon(class_icons[i].icon);
		if (ch.res_name)
			XFree(ch.res_name);
		if (ch.res_class)
			XFree(ch.res_class);
	}
	return img ? img : theme_icon("application-x-executable");
}

static void free_task_icon(struct task *t)
{
	if (t->icon && t->own_icon) {
		imlib_context_set_image(t->icon);
		imlib_free_image();
	}
	t->icon = NULL;
	t->own_icon = 0;
}

static void load_task_icon(struct task *t)
{
	free_task_icon(t);
	t->icon = net_wm_icon(t->win);
	t->own_icon = t->icon != NULL;
	if (!t->icon)
		t->icon = class_icon(t->win);
}

/* Draws an icon at ICON_SIZE, blended over what is already in the pixmap. */
static void draw_icon(Drawable d, Imlib_Image img, int x, int y)
{
	if (!img)
		return;
	imlib_context_set_drawable(d);
	imlib_context_set_image(img);
	imlib_context_set_blend(1);
	imlib_render_image_on_drawable_at_size(x, y, ICON_SIZE, ICON_SIZE);
}

/* Rebuild the task list from what Openbox publishes on the root window. */
static void refresh_tasks(void)
{
	unsigned long n = 0;
	Window *list = get_prop(root, A_CLIENT_LIST, XA_WINDOW, &n);
	Window *act = get_prop(root, A_ACTIVE, XA_WINDOW, NULL);
	static struct task old[MAX_TASKS];
	int nold = ntasks;

	/* keep the icons of windows that are still there */
	memcpy(old, tasks, sizeof old);

	active = act ? *act : None;
	if (act)
		XFree(act);

	ntasks = 0;
	for (unsigned long i = 0; list && i < n && ntasks < MAX_TASKS; i++) {
		Window w = list[i];

		if (w == panel || has_atom(w, A_TYPE, A_TYPE_DOCK) ||
		    has_atom(w, A_TYPE, A_TYPE_DESKTOP) ||
		    has_atom(w, A_STATE, A_SKIP_TASKBAR))
			continue;
		/* so we hear when its title or minimised state changes */
		XSelectInput(dpy, w, PropertyChangeMask);

		struct task *t = &tasks[ntasks++];
		t->win = w;
		t->hidden = has_atom(w, A_STATE, A_HIDDEN);
		t->icon = NULL;
		t->own_icon = 0;
		window_title(w, t->title);
		for (int j = 0; j < nold; j++)
			if (old[j].win == w) {
				t->icon = old[j].icon;
				t->own_icon = old[j].own_icon;
				old[j].win = None;          /* taken over */
			}
		if (!t->icon)
			load_task_icon(t);
	}
	if (list)
		XFree(list);
	for (int j = 0; j < nold; j++)          /* windows that were closed */
		if (old[j].win != None)
			free_task_icon(&old[j]);
}

static int text_width(XftFont *f, const char *s)
{
	XGlyphInfo g;

	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &g);
	return g.xOff;
}

/* Copies s into out, shortened with "â€¦" until it fits into maxw pixels. */
static void fit_text(XftFont *f, const char *s, int maxw, char *out)
{
	size_t len;

	snprintf(out, TITLE_MAX + 1, "%s", s);
	if (text_width(f, out) <= maxw)
		return;
	len = strlen(out);
	while (len > 0) {
		do                      /* step back one UTF-8 character */
			len--;
		while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
		strcpy(out + len, "\xe2\x80\xa6");   /* "â€¦" */
		if (text_width(f, out) <= maxw)
			return;
	}
	out[0] = '\0';
}

static void text(XftDraw *d, XftFont *f, XftColor *c, int x, int top, int h,
                 const char *s)
{
	int y = top + (h - (f->ascent + f->descent)) / 2 + f->ascent;

	XftDrawStringUtf8(d, c, f, x, y, (const FcChar8 *)s, strlen(s));
}

/* Recomputes the clock's text. Returns 1 if what the panel shows changed. */
static int update_clock(void)
{
	char old_time[sizeof clock_text], old_date[sizeof date_text];
	struct tm tm;

	strcpy(old_time, clock_text);
	strcpy(old_date, date_text);
	clock_now = time(NULL);
	tzset();         /* the time zone may have changed (Users and system) */
	localtime_r(&clock_now, &tm);
	settings_format_time(&cfg, &tm, clock_text, sizeof clock_text);
	settings_format_date(&cfg, &tm, date_text, sizeof date_text);
	if (!cfg.clock_date)
		date_text[0] = '\0';
	return strcmp(old_time, clock_text) != 0 || strcmp(old_date, date_text) != 0;
}

/* Draws the analog clock face (settings/dial.c) into a pixmap. */
static void draw_dial(Drawable d, int w, int h, double cx, double cy, double r,
                      time_t now, int seconds)
{
	cairo_surface_t *surf = cairo_xlib_surface_create(dpy, d, vis, w, h);
	cairo_t *cr = cairo_create(surf);
	struct tm tm;

	localtime_r(&now, &tm);
	dial_draw(cr, &cfg.pal, cx, cy, r, &tm, seconds);
	cairo_destroy(cr);
	cairo_surface_destroy(surf);
}

/* Buttons wide enough get an x that closes the window. */
static int has_close(const struct task *t)
{
	return t->w >= ICON_SIZE + 16 + CLOSE_W + 40;
}

/* Where the x is: a square at the right end of the button */
static int close_x(const struct task *t)
{
	return t->x + t->w - CLOSE_W - 4;
}

/* The x, with a red circle behind it while the mouse is on it */
static void draw_close(const struct task *t, int hot)
{
	cairo_surface_t *surf = cairo_xlib_surface_create(dpy, buf, vis, sw, panel_h);
	cairo_t *cr = cairo_create(surf);
	double cx = close_x(t) + CLOSE_W / 2.0, cy = panel_h / 2.0, r = 4.5;

	XFlush(dpy);                 /* Xft's drawing first, then cairo's on top */
	if (hot) {
		cairo_arc(cr, cx, cy, CLOSE_W / 2.0 - 1, 0, 2 * M_PI);
		dial_color(cr, "#e5484d", 1);
		cairo_fill(cr);
	}
	cairo_move_to(cr, cx - r, cy - r);
	cairo_line_to(cr, cx + r, cy + r);
	cairo_move_to(cr, cx + r, cy - r);
	cairo_line_to(cr, cx - r, cy + r);
	cairo_set_line_width(cr, 1.6);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	dial_color(cr, hot ? "#ffffff" : cfg.pal.text, hot ? 1 : 0.8);
	cairo_stroke(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surf);
}

/* Is x on the x of task i? */
static int on_close(int i, int x)
{
	return i >= 0 && has_close(&tasks[i]) && x >= close_x(&tasks[i]) &&
	       x < close_x(&tasks[i]) + CLOSE_W;
}

/*
 * The clock area at the right end of the panel. Digital: time in bold with
 * the date small underneath (or the time alone). Analog: a small dial, with
 * the date next to it. Sets clock_x / clock_w.
 */
static void draw_clock(void)
{
	int bh = panel_h - 2 * PAD;
	int two_lines = date_text[0] && bold->ascent + bold->descent +
	                small->ascent + small->descent <= panel_h - 2;
	int tw = text_width(bold, clock_text), dw = text_width(small, date_text);
	int dial = bh - 4;

	if (cfg.panel_clock == CLOCK_ANALOG)
		clock_w = dial + (date_text[0] ? text_width(font, date_text) + 12 : 0) + 24;
	else if (two_lines)
		clock_w = (tw > dw ? tw : dw) + 28;
	else
		clock_w = tw + (date_text[0] ? text_width(font, date_text) + 14 : 0) + 28;
	clock_x = sw - PAD - clock_w;

	XftDrawRect(draw, cal_open ? &c_active : hover == HOVER_CLOCK ? &c_hover
	            : &c_bg, clock_x, PAD, clock_w, bh);
	if (cal_open)
		XftDrawRect(draw, &c_accent, clock_x, cfg.panel_position == PANEL_TOP
		            ? PAD : PAD + bh - 2, clock_w, 2);

	if (cfg.panel_clock == CLOCK_ANALOG) {
		int x = clock_x + 12;

		if (date_text[0]) {
			text(draw, font, &c_text, x, PAD, bh, date_text);
			x += text_width(font, date_text) + 12;
		}
		XFlush(dpy);    /* Xft's drawing first, then cairo's on top */
		draw_dial(buf, sw, panel_h, x + dial / 2.0, panel_h / 2.0, dial / 2.0 - 1,
		          clock_now, cfg.clock_seconds);
	} else if (two_lines) {
		int th = bold->ascent + bold->descent, dh = small->ascent + small->descent;
		int top = (panel_h - th - dh) / 2;
		int cx = clock_x + clock_w / 2;

		text(draw, bold, &c_text, cx - tw / 2, top, th, clock_text);
		text(draw, small, &c_dim, cx - dw / 2, top + th, dh, date_text);
	} else {
		int x = clock_x + 14;

		if (date_text[0]) {
			text(draw, font, &c_dim, x, PAD, bh, date_text);
			x += text_width(font, date_text) + 14;
		}
		text(draw, bold, &c_text, x, PAD, bh, clock_text);
	}
}

/* Reads ~/.cache/my-distro/update when it changed ("available = 0.9").
 * Returns 1 if the button has to be drawn again. */
static int read_update_status(void)
{
	char path[512], line[128], found[32] = "";
	const char *home = getenv("HOME");
	struct stat st;
	FILE *f;

	snprintf(path, sizeof path, "%s/%s", home ? home : "/", UPDATE_FILE);
	if (stat(path, &st) != 0)
		st.st_mtime = 0;
	if (st.st_mtime == update_mtime)
		return 0;
	update_mtime = st.st_mtime;
	if ((f = fopen(path, "r"))) {
		while (fgets(line, sizeof line, f))
			if (sscanf(line, " available = %31s", found) == 1)
				break;
		fclose(f);
	}
	if (strcmp(found, update_version) == 0)
		return 0;
	snprintf(update_version, sizeof update_version, "%s", found);
	return 1;
}

/* The "Update to X" button, left of the clock. Sets update_x / update_w. */
static void draw_update(void)
{
	char label[64];
	int bh = panel_h - 2 * PAD;

	update_w = 0;
	if (!update_version[0])
		return;
	snprintf(label, sizeof label, "\xe2\x86\x91  Update to %s", update_version);  /* ↑ */
	update_w = text_width(bold, label) + 24;
	update_x = clock_x - PAD - update_w;
	XftDrawRect(draw, hover == HOVER_UPDATE ? &c_active : &c_accent,
	            update_x, PAD, update_w, bh);
	text(draw, bold, hover == HOVER_UPDATE ? &c_text : &c_on_accent,
	     update_x + 12, PAD, bh, label);
}

/*
 * Wi-Fi: /sys/class/ieee80211 has one entry per Wi-Fi adapter. The signal
 * comes from /proc/net/wireless, which lists an interface only while it is
 * connected:
 *   wlan0: 0000   54.  -56.  -256 ...
 * the 54 is the link quality out of 70. Returns 1 when the bars have to be
 * drawn again.
 */
static int read_wifi(void)
{
	char line[256], name[32];
	float link;
	int level = -2, n = 0;
	DIR *d = opendir("/sys/class/ieee80211");
	struct dirent *e;
	FILE *f;

	while (d && level == -2 && (e = readdir(d)))
		if (e->d_name[0] != '.')
			level = -1;                  /* an adapter, not connected */
	if (d)
		closedir(d);
	f = level == -2 ? NULL : fopen("/proc/net/wireless", "r");

	while (f && fgets(line, sizeof line, f))
		if (++n > 2 && sscanf(line, " %31[^:]: %*x %f", name, &link) == 2) {
			int l = link <= 0 ? -1 : (int)(link * 4 / 70 + 0.99);

			l = l > 4 ? 4 : l;
			level = l > level ? l : level;
		}
	if (f)
		fclose(f);
	if (level == wifi_level)
		return 0;
	wifi_level = level;
	return 1;
}

/* Four bars, left of the update button / clock. Sets wifi_x / wifi_w. */
static void draw_wifi(int right)
{
	int bh = panel_h - 2 * PAD, bw = 4, gap = 2, top = PAD + 7, h = bh - 14;

	wifi_w = 0;
	if (wifi_level == -2)
		return;
	wifi_w = 4 * bw + 3 * gap + 20;
	wifi_x = right - PAD - wifi_w;
	XftDrawRect(draw, hover == HOVER_WIFI ? &c_hover : &c_bg, wifi_x, PAD, wifi_w, bh);
	for (int i = 0; i < 4; i++) {
		int bar = h * (i + 1) / 4;

		XftDrawRect(draw, i < wifi_level ? &c_text : &c_sep,
		            wifi_x + 10 + i * (bw + gap), top + h - bar, bw, bar);
	}
}

static void run(const char *cmd);

/* A cairo context for drawing into the panel's pixmap, on top of Xft */
static cairo_t *panel_cairo(void)
{
	cairo_surface_t *surf = cairo_xlib_surface_create(dpy, buf, vis, sw, panel_h);
	cairo_t *cr = cairo_create(surf);

	cairo_surface_destroy(surf);     /* cr keeps it alive */
	XFlush(dpy);                     /* Xft's drawing first */
	return cr;
}

/* --- Sound ------------------------------------------------------------------ */

static void close_mixer(void)
{
	if (mixer)
		snd_mixer_close(mixer);
	mixer = NULL;
	vol_elem = NULL;
}

/*
 * Opens the mixer of the default sound card and picks the control the
 * volume keys change too (my-volume): Master, PCM, Speaker, Headphone, or
 * else the first one with a playback volume. Returns 1 if there is one.
 */
static int open_mixer(void)
{
	static const char *names[] = { "Master", "PCM", "Speaker", "Headphone" };
	snd_mixer_elem_t *e;

	if (snd_mixer_open(&mixer, 0) < 0) {
		mixer = NULL;
		return 0;
	}
	if (snd_mixer_attach(mixer, "default") < 0 ||
	    snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
	    snd_mixer_load(mixer) < 0) {
		close_mixer();
		return 0;
	}
	for (size_t i = 0; !vol_elem && i < sizeof names / sizeof *names; i++)
		for (e = snd_mixer_first_elem(mixer); e; e = snd_mixer_elem_next(e))
			if (strcmp(snd_mixer_selem_get_name(e), names[i]) == 0 &&
			    snd_mixer_selem_get_index(e) == 0 &&
			    snd_mixer_selem_has_playback_volume(e)) {
				vol_elem = e;
				break;
			}
	for (e = snd_mixer_first_elem(mixer); !vol_elem && e; e = snd_mixer_elem_next(e))
		if (snd_mixer_selem_has_playback_volume(e))
			vol_elem = e;
	if (!vol_elem)
		close_mixer();
	return vol_elem != NULL;
}

static void volume_path(char *path, size_t len)
{
	const char *home = getenv("HOME");

	snprintf(path, len, "%s/%s", home ? home : "/", VOLUME_FILE);
}

/* Remembers the volume for the next login: "70 on" */
static void save_volume(void)
{
	char path[512], tmp[520];
	FILE *f;

	volume_path(path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s", path);
	*strrchr(tmp, '/') = '\0';           /* ~/.config/my-distro */
	*strrchr(tmp, '/') = '\0';           /* ~/.config */
	mkdir(tmp, 0755);
	snprintf(tmp, sizeof tmp, "%s", path);
	*strrchr(tmp, '/') = '\0';
	mkdir(tmp, 0755);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	if ((f = fopen(tmp, "w"))) {
		fprintf(f, "%d %s\n", vol_pct, vol_on ? "on" : "off");
		if (fclose(f) == 0)
			rename(tmp, path);
	}
}

/* Sets the volume in percent (0-100) and, if on >= 0, sound on / off. */
static void set_volume(int pct, int on)
{
	long min, max;

	if (!vol_elem)
		return;
	pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
	snd_mixer_selem_get_playback_volume_range(vol_elem, &min, &max);
	snd_mixer_selem_set_playback_volume_all(vol_elem, min + (max - min) * pct / 100);
	if (on >= 0 && snd_mixer_selem_has_playback_switch(vol_elem))
		snd_mixer_selem_set_playback_switch_all(vol_elem, on);
}

/*
 * Reads the volume (it can also change from outside: volume keys,
 * alsamixer). Returns 1 when the speaker has to be drawn again.
 */
static int read_volume(void)
{
	long min, max, v = 0;
	int sw_on = 1, pct, changed, save;

	if (!mixer) {
		time_t now = time(NULL);

		/* no sound card yet: a USB one may come; look again now and then */
		if (now - vol_retry < 10)
			return 0;
		vol_retry = now;
		if (!open_mixer()) {
			changed = vol_pct != -1;
			vol_pct = -1;
			return changed;
		}
	} else {
		struct pollfd pfd[8];
		int n = snd_mixer_poll_descriptors(mixer, pfd, 8);

		/* handle_events would wait when nothing happened: ask poll first */
		if (n > 0 && poll(pfd, n, 0) > 0 && snd_mixer_handle_events(mixer) < 0) {
			close_mixer();                /* the card is gone */
			vol_pct = -1;
			return 1;
		}
	}
	snd_mixer_selem_get_playback_volume_range(vol_elem, &min, &max);
	snd_mixer_selem_get_playback_volume(vol_elem, SND_MIXER_SCHN_FRONT_LEFT, &v);
	if (snd_mixer_selem_has_playback_switch(vol_elem))
		snd_mixer_selem_get_playback_switch(vol_elem, SND_MIXER_SCHN_FRONT_LEFT, &sw_on);
	pct = max > min ? (int)(((v - min) * 100 + (max - min) / 2) / (max - min)) : 0;
	changed = pct != vol_pct || !!sw_on != vol_on;
	save = changed && vol_pct != -1;      /* changed by you, not the first read */
	vol_pct = pct;
	vol_on = !!sw_on;
	if (save)
		save_volume();
	return changed;
}

/* At login: your volume from last time */
static void restore_volume(void)
{
	char path[512], on[8] = "on";
	int pct;
	FILE *f;

	volume_path(path, sizeof path);
	if (!vol_elem || !(f = fopen(path, "r")))
		return;
	if (fscanf(f, "%d %7s", &pct, on) >= 1)
		set_volume(pct, strcmp(on, "off") != 0);
	fclose(f);
}

/* Speaker with waves (or a cross when muted) and the percent. */
static void draw_volume(int right)
{
	char label[16];
	int bh = panel_h - 2 * PAD, tw;
	double cx, cy = panel_h / 2.0, s = 1.0;
	cairo_t *cr;

	vol_w = 0;
	if (vol_pct < 0)
		return;
	snprintf(label, sizeof label, "%d%%", vol_pct);
	tw = text_width(small, label);
	vol_w = 18 + tw + 26;
	vol_x = right - PAD - vol_w;
	XftDrawRect(draw, hover == HOVER_VOLUME ? &c_hover : &c_bg, vol_x, PAD, vol_w, bh);
	text(draw, small, vol_on ? &c_dim : &c_sep, vol_x + vol_w - 8 - tw, PAD, bh, label);

	cr = panel_cairo();
	cx = vol_x + 10;
	/* the speaker: a small box and a cone */
	cairo_move_to(cr, cx, cy - 2.5 * s);
	cairo_line_to(cr, cx + 3 * s, cy - 2.5 * s);
	cairo_line_to(cr, cx + 7 * s, cy - 6 * s);
	cairo_line_to(cr, cx + 7 * s, cy + 6 * s);
	cairo_line_to(cr, cx + 3 * s, cy + 2.5 * s);
	cairo_line_to(cr, cx, cy + 2.5 * s);
	cairo_close_path(cr);
	dial_color(cr, vol_on ? cfg.pal.text : cfg.pal.dim, 1);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1.4);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (!vol_on || vol_pct == 0) {        /* x */
		cairo_move_to(cr, cx + 10 * s, cy - 3 * s);
		cairo_line_to(cr, cx + 16 * s, cy + 3 * s);
		cairo_move_to(cr, cx + 16 * s, cy - 3 * s);
		cairo_line_to(cr, cx + 10 * s, cy + 3 * s);
		dial_color(cr, vol_on ? cfg.pal.text : "#e5484d", 1);
		cairo_stroke(cr);
	} else {                              /* 1-3 waves */
		int waves = vol_pct < 34 ? 1 : vol_pct < 67 ? 2 : 3;

		for (int i = 0; i < 3; i++) {
			cairo_new_sub_path(cr);
			cairo_arc(cr, cx + 7 * s, cy, (3.5 + i * 3) * s, -M_PI / 3.2, M_PI / 3.2);
			dial_color(cr, i < waves ? cfg.pal.text : cfg.pal.sep, 1);
			cairo_stroke(cr);
		}
	}
	cairo_destroy(cr);
}

/* --- Battery ------------------------------------------------------------------ */

/*
 * /sys/class/power_supply has one folder per battery (type "Battery") with
 * its charge in percent (capacity) and status (Charging, Discharging, Full,
 * Not charging). With two batteries we show the average. Returns 1 when the
 * battery has to be drawn again.
 */
static int read_battery(void)
{
	char path[300], line[64];
	int sum = 0, n = 0, charging = 0, pct, changed;
	time_t now = time(NULL);
	DIR *d;
	struct dirent *e;
	FILE *f;

	if (now - bat_read < 5)              /* changes slowly */
		return 0;
	bat_read = now;
	if ((d = opendir("/sys/class/power_supply"))) {
		while ((e = readdir(d))) {
			int cap = -1;

			if (e->d_name[0] == '.')
				continue;
			snprintf(path, sizeof path, "/sys/class/power_supply/%s/type", e->d_name);
			if (!(f = fopen(path, "r")))
				continue;
			line[0] = '\0';
			if (!fgets(line, sizeof line, f))
				line[0] = '\0';
			fclose(f);
			if (strncmp(line, "Battery", 7) != 0)
				continue;
			/* HID devices (wireless mice) report a battery too: skip them */
			snprintf(path, sizeof path, "/sys/class/power_supply/%s/scope", e->d_name);
			if ((f = fopen(path, "r"))) {
				int device = fgets(line, sizeof line, f) && strncmp(line, "Device", 6) == 0;

				fclose(f);
				if (device)
					continue;
			}
			snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", e->d_name);
			if ((f = fopen(path, "r"))) {
				if (fscanf(f, "%d", &cap) != 1)
					cap = -1;
				fclose(f);
			}
			if (cap < 0)
				continue;
			sum += cap > 100 ? 100 : cap;
			n++;
			snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", e->d_name);
			if ((f = fopen(path, "r"))) {
				if (fgets(line, sizeof line, f) && strncmp(line, "Discharging", 11) != 0)
					charging = 1;     /* charging, full, or plugged in */
				fclose(f);
			}
		}
		closedir(d);
	}
	pct = n ? sum / n : -1;

	if (pct >= 0 && pct <= BATTERY_LOW && !charging && !bat_warned) {
		char cmd[256];

		snprintf(cmd, sizeof cmd, "my-notify -u critical -i battery-caution "
		         "'Battery low: %d %%' 'Plug in the charger, or save your work.'", pct);
		run(cmd);
		bat_warned = 1;
	}
	if (charging || pct > BATTERY_LOW)
		bat_warned = 0;

	changed = pct != bat_pct || charging != bat_charging;
	bat_pct = pct;
	bat_charging = charging;
	return changed;
}

/* A battery outline filled up to its charge, and the percent. */
static void draw_battery(int right)
{
	char label[16];
	int bh = panel_h - 2 * PAD, tw;
	double bw = 22, bht = 11, x, y = panel_h / 2.0 - bht / 2;
	const char *fill = bat_charging ? cfg.pal.accent :
	                   bat_pct <= BATTERY_LOW * 2 ? "#e5484d" : cfg.pal.text;
	cairo_t *cr;

	bat_w = 0;
	if (bat_pct < 0)
		return;
	snprintf(label, sizeof label, "%d%%", bat_pct);
	tw = text_width(small, label);
	bat_w = (int)bw + 3 + tw + 26;
	bat_x = right - PAD - bat_w;
	XftDrawRect(draw, hover == HOVER_BATTERY ? &c_hover : &c_bg, bat_x, PAD, bat_w, bh);
	text(draw, small, &c_dim, bat_x + bat_w - 8 - tw, PAD, bh, label);

	cr = panel_cairo();
	x = bat_x + 10.5;
	cairo_rectangle(cr, x, y + 0.5, bw, bht);           /* outline */
	cairo_set_line_width(cr, 1.2);
	dial_color(cr, cfg.pal.text, 0.9);
	cairo_stroke(cr);
	cairo_rectangle(cr, x + bw + 0.5, y + 3.5, 2.5, bht - 6);   /* the nub */
	cairo_fill(cr);
	cairo_rectangle(cr, x + 2, y + 2.5, (bw - 4) * bat_pct / 100.0, bht - 4);
	dial_color(cr, fill, 1);
	cairo_fill(cr);
	if (bat_charging) {                                  /* a small lightning bolt */
		double mx = x + bw / 2, my = y + bht / 2 + 0.5;

		cairo_move_to(cr, mx + 1.5, my - 5);
		cairo_line_to(cr, mx - 2.5, my + 0.8);
		cairo_line_to(cr, mx, my + 0.8);
		cairo_line_to(cr, mx - 1.5, my + 5);
		cairo_line_to(cr, mx + 2.5, my - 0.8);
		cairo_line_to(cr, mx, my - 0.8);
		cairo_close_path(cr);
		dial_color(cr, cfg.pal.on_accent, 1);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
}

static void draw_panel(void)
{
	char label[TITLE_MAX + 8];
	int bh = panel_h - 2 * PAD;
	int left = PAD + START_W + 2 * PAD;
	int avail, right;

	XftDrawRect(draw, &c_bg, 0, 0, sw, panel_h);
	/* thin accent line on the edge towards the screen */
	XftDrawRect(draw, &c_accent, 0, cfg.panel_position == PANEL_TOP ? panel_h - 1 : 0,
	            sw, 1);

	/* From the right: clock, update button, Wi-Fi, volume, battery. Each
	 * one starts left of the one before (the ones not shown take no room). */
	draw_clock();
	draw_update();
	right = update_w ? update_x : clock_x;
	draw_wifi(right);
	right = wifi_w ? wifi_x : right;
	draw_volume(right);
	right = vol_w ? vol_x : right;
	draw_battery(right);
	right = bat_w ? bat_x : right;
	avail = right - PAD - left;

	/* Start button */
	XftDrawRect(draw, menu_open ? &c_active : hover == HOVER_START ? &c_hover
	            : &c_button, PAD, PAD, START_W, bh);
	text(draw, bold, &c_accent, PAD + 12, PAD, bh, "\xe2\x98\xb0");  /* â˜° */
	text(draw, bold, &c_text, PAD + 34, PAD, bh, "Start");

	/* One button per window */
	if (ntasks > 0) {
		int w = (avail - (ntasks - 1) * PAD) / ntasks;

		if (w > TASK_MAX_W)
			w = TASK_MAX_W;
		if (w < ICON_SIZE + 16)
			w = ICON_SIZE + 16;
		for (int i = 0; i < ntasks; i++) {
			struct task *t = &tasks[i];
			int is_active = t->win == active && !t->hidden;

			t->x = left + i * (w + PAD);
			t->w = w;
			XftDrawRect(draw, is_active ? &c_active : hover == i ? &c_hover
			            : &c_button, t->x, PAD, w, bh);
			if (is_active)
				XftDrawRect(draw, &c_accent, t->x, PAD + bh - 2, w, 2);
			draw_icon(buf, t->icon, t->x + 8, PAD + (bh - ICON_SIZE) / 2);
			fit_text(font, t->title, w - ICON_SIZE - 26 - (has_close(t) ? CLOSE_W : 0),
			         label);
			text(draw, font, t->hidden ? &c_dim : &c_text,
			     t->x + ICON_SIZE + 16, PAD, bh, label);
			if (has_close(t) && (hover == i || is_active))
				draw_close(t, hover == i && hover_close);
		}
	}

	XCopyArea(dpy, buf, panel, DefaultGC(dpy, scr), 0, 0, sw, panel_h, 0, 0);
}

static void draw_menu(void)
{
	XftDrawRect(menu_draw, &c_bg, 0, 0, menu_w, menu_h);
	XftDrawRect(menu_draw, &c_button, 0, 0, menu_w, HEADER_H);
	XftDrawRect(menu_draw, &c_accent, 0, HEADER_H - 2, menu_w, 2);
	text(menu_draw, bold, &c_accent, 16, 0, HEADER_H, "my-distro");
	for (int x = MENU_W; x < menu_w; x += MENU_W)   /* lines between columns */
		XftDrawRect(menu_draw, &c_sep, x, HEADER_H + 6, 1, menu_h - HEADER_H - 12);

	for (int i = 0; i < nitems; i++) {
		struct item *it = &items[i];

		if (!it->cmd[0]) {
			if (it->h)
				XftDrawRect(menu_draw, &c_sep, it->x + 12, it->y + it->h / 2,
				            MENU_W - 24, 1);
			continue;
		}
		if (i == menu_hover)
			XftDrawRect(menu_draw, &c_hover, it->x, it->y, MENU_W, it->h);
		draw_icon(menu_buf, it->icon, it->x + 16, it->y + (it->h - ICON_SIZE) / 2);
		text(menu_draw, font, &c_text, it->x + 16 + ICON_SIZE + 12, it->y, it->h,
		     it->label);
	}
	/* 1-pixel border */
	XftDrawRect(menu_draw, &c_accent, 0, 0, menu_w, 1);
	XftDrawRect(menu_draw, &c_accent, 0, 0, 1, menu_h);
	XftDrawRect(menu_draw, &c_accent, menu_w - 1, 0, 1, menu_h);
}

static void add_item(const char *label, const char *cmd, const char *icon)
{
	struct item *it;

	if (nitems == MAX_ITEMS)
		return;
	it = &items[nitems++];
	snprintf(it->label, sizeof it->label, "%s", label);
	snprintf(it->cmd, sizeof it->cmd, "%s", cmd);
	it->icon = theme_icon(icon);
}

/* Adds the entries of a menu file. With stop_at_line set, it stops after the
 * first "-" line and returns 1 (0: the file ended or is missing). */
static int read_menu_file(FILE *f, int stop_at_line)
{
	char line[400];

	while (f && fgets(line, sizeof line, f)) {
		char *bar;

		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		if (strcmp(line, "-") == 0) {
			add_item("", "", NULL);
			if (stop_at_line)
				return 1;
			continue;
		}
		bar = strchr(line, '|');
		if (bar) {
			char *icon = strchr(bar + 1, '|');   /* optional 3rd field */

			*bar = '\0';
			if (icon)
				*icon++ = '\0';
			add_item(line, bar + 1, icon);
		}
	}
	return 0;
}

/* The menu file's first group (the apps), the Software app's programs in a
 * group of their own, then the rest of the menu file. */
static void load_menu(void)
{
	FILE *f = fopen(MENU_FILE, "r");
	FILE *pkg = fopen(PKG_MENU, "r");
	int before;

	nitems = 0;
	if (read_menu_file(f, 1) && pkg) {
		before = nitems;
		read_menu_file(pkg, 0);
		if (nitems > before)
			add_item("", "", NULL);     /* line after the programs */
	}
	read_menu_file(f, 0);
	if (f)
		fclose(f);
	if (pkg)
		fclose(pkg);
	if (nitems == 0)
		add_item("Terminal", "xterm", "utilities-terminal");
}

static time_t pkg_menu_mtime(void)
{
	struct stat st;

	return stat(PKG_MENU, &st) == 0 ? st.st_mtime : 0;
}

/* Places the entries in ncols columns: a new column starts when the next
 * entry would make this one taller than `target` pixels, or at a separator
 * close to that (the separator is then not drawn). Sets menu_w and menu_h. */
static void place_items(int ncols, int target)
{
	int top = HEADER_H + 6, y = top, col = 0, bottom = top;

	for (int i = 0; i < nitems; i++) {
		int sep = !items[i].cmd[0];
		int h = sep ? SEP_H : item_h;

		if (col < ncols - 1 && y > top &&
		    (y + h > top + target || (sep && y - top > target * 3 / 4))) {
			col++;
			y = top;
		}
		if (sep && y == top)
			h = 0;                  /* no line at the top of a column */
		items[i].x = col * MENU_W;
		items[i].y = y;
		items[i].h = h;
		y += h;
		if (y > bottom)
			bottom = y;
	}
	menu_w = (col + 1) * MENU_W;
	menu_h = bottom + 6;
}

/* One column, or as many as it takes to fit on the screen (above or below
 * the panel). */
static void layout_menu(void)
{
	int body = 0, max_h = sh - panel_h - 2 * PAD;

	for (int i = 0; i < nitems; i++)
		body += items[i].cmd[0] ? item_h : SEP_H;
	for (int ncols = 1; ncols <= 8; ncols++) {
		place_items(ncols, (body + ncols - 1) / ncols);
		if (menu_h <= max_h)
			return;
	}
}

/* (Re)makes the menu's picture in its current size */
static void size_menu_window(void)
{
	if (menu_draw)
		XftDrawDestroy(menu_draw);
	if (menu_buf)
		XFreePixmap(dpy, menu_buf);
	XResizeWindow(dpy, menu, menu_w, menu_h);
	menu_buf = XCreatePixmap(dpy, menu, menu_w, menu_h, DefaultDepth(dpy, scr));
	menu_draw = XftDrawCreate(dpy, menu_buf, vis, cmap);
	XSetWindowBackgroundPixmap(dpy, menu, menu_buf);
}

static void run(const char *cmd)
{
	if (fork() == 0) {
		setsid();
		/* We ignore SIGCHLD (so our children are reaped by themselves), and a
		 * started program would inherit that: then it couldn't wait for its
		 * own children (Users and system for adduser, the lock screen for
		 * the password check). Give it the normal handling back. */
		signal(SIGCHLD, SIG_DFL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static void open_menu(void)
{
	XMoveWindow(dpy, menu, PAD, cfg.panel_position == PANEL_TOP ? panel_h
	            : sh - panel_h - menu_h);
	XMapRaised(dpy, menu);
	/* Grab mouse and keyboard so a click anywhere else closes the menu.
	 * Right after a keyboard shortcut Openbox may still hold the keyboard,
	 * so try a few times. */
	for (int i = 0; i < 20; i++) {
		if (XGrabKeyboard(dpy, menu, False, GrabModeAsync, GrabModeAsync,
		                  CurrentTime) == GrabSuccess)
			break;
		usleep(10000);
	}
	XGrabPointer(dpy, menu, False, ButtonPressMask | ButtonReleaseMask |
	             PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None,
	             CurrentTime);
	menu_open = 1;
	menu_hover = -1;
}

static void close_menu(void)
{
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XUnmapWindow(dpy, menu);
	menu_open = 0;
}

static int menu_item_at(int x, int y)
{
	for (int i = 0; i < nitems; i++)
		if (items[i].cmd[0] && x >= items[i].x && x < items[i].x + MENU_W &&
		    y >= items[i].y && y < items[i].y + items[i].h)
			return i;
	return -1;
}

/* Moves the keyboard selection to the next real item (skips separators). */
static void menu_step(int dir)
{
	int i = menu_hover;

	for (int n = 0; n < nitems; n++) {
		i = (i + dir + nitems) % nitems;
		if (i < 0)
			i = nitems - 1;
		if (items[i].cmd[0]) {
			menu_hover = i;
			return;
		}
	}
}

static void activate(Window w)
{
	XEvent e = { 0 };

	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = A_ACTIVE;
	e.xclient.format = 32;
	e.xclient.data.l[0] = 2;        /* 2 = request comes from a pager/taskbar */
	e.xclient.data.l[1] = CurrentTime;
	XSendEvent(dpy, root, False,
	           SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

/* --- Calendar popup (click the clock) ------------------------------------- */

static void fill_circle(Drawable d, int w, int h, double cx, double cy, double r,
                        const char *hex)
{
	cairo_surface_t *surf = cairo_xlib_surface_create(dpy, d, vis, w, h);
	cairo_t *cr = cairo_create(surf);

	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	dial_color(cr, hex, 1);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surf);
}

/* Draws s centred on x */
static void text_c(XftDraw *d, XftFont *f, XftColor *c, int x, int top, int h,
                   const char *s)
{
	text(d, f, c, x - text_width(f, s) / 2, top, h, s);
}

/* The "<" and ">" buttons next to the month name */
static int cal_arrow_x(int which)
{
	return CAL_W - 84 + which * 38;
}

#define CAL_MONTH_Y  (CAL_TOP + 10)
#define CAL_GRID_Y   (CAL_MONTH_Y + 36 + 26)
#define CAL_CELL_W   ((CAL_W - 24) / 7)

static void draw_calendar(void)
{
	char s[96];
	time_t t = time(NULL);
	struct tm now, first = { 0 }, last = { 0 };
	int off, days;

	localtime_r(&t, &now);
	XftDrawRect(cal_draw, &c_bg, 0, 0, CAL_W, cal_h);

	/* big time and the full date */
	settings_format_time(&cfg, &now, s, sizeof s);
	text(cal_draw, big, &c_text, 18, 10, 50, s);
	strftime(s, sizeof s, "%A, %d %B %Y", &now);
	text(cal_draw, font, &c_accent, 20, 58, 24, s);
	XftDrawRect(cal_draw, &c_sep, 12, CAL_TOP - 1, CAL_W - 24, 1);

	/* month name and the arrows */
	first.tm_year = cal_year - 1900;
	first.tm_mon = cal_month;
	first.tm_mday = 1;
	first.tm_hour = 12;
	mktime(&first);                       /* fills in tm_wday */
	last = first;
	last.tm_mon++;
	last.tm_mday = 0;                     /* day 0 of next month = last day */
	mktime(&last);
	days = last.tm_mday;
	off = (first.tm_wday + 6) % 7;        /* weeks start on Monday */

	strftime(s, sizeof s, "%B %Y", &first);
	text(cal_draw, bold, &c_text, 20, CAL_MONTH_Y, 32, s);
	for (int i = 0; i < 2; i++) {
		if (cal_hover == i)
			XftDrawRect(cal_draw, &c_hover, cal_arrow_x(i), CAL_MONTH_Y, 32, 32);
		text_c(cal_draw, bold, &c_accent, cal_arrow_x(i) + 16, CAL_MONTH_Y, 32,
		       i == 0 ? "\xe2\x80\xb9" : "\xe2\x80\xba");      /* "<" ">" */
	}

	/* weekday names, Monday first: 5 Jan 1970 was a Monday */
	for (int i = 0; i < 7; i++) {
		struct tm wd = { .tm_wday = (i + 1) % 7 };
		char name[16];

		strftime(name, sizeof name, "%a", &wd);
		name[2] = '\0';
		text_c(cal_draw, small, i >= 5 ? &c_accent : &c_dim,
		       12 + i * CAL_CELL_W + CAL_CELL_W / 2, CAL_GRID_Y - 26, 22, name);
	}

	/* the days */
	XFlush(dpy);
	for (int d = 1; d <= days; d++) {
		int col = (off + d - 1) % 7, row = (off + d - 1) / 7;
		int cx = 12 + col * CAL_CELL_W + CAL_CELL_W / 2;
		int top = CAL_GRID_Y + row * CAL_CELL;
		int today = d == now.tm_mday && cal_month == now.tm_mon &&
		            cal_year == now.tm_year + 1900;

		snprintf(s, sizeof s, "%d", d);
		if (today)
			fill_circle(cal_buf, CAL_W, cal_h, cx, top + CAL_CELL / 2.0,
			            CAL_CELL / 2.0 - 3, cfg.pal.accent);
		text_c(cal_draw, today ? bold : font,
		       today ? &c_on_accent : col >= 5 ? &c_dim : &c_text,
		       cx, top, CAL_CELL, s);
	}

	/* 1-pixel border */
	XftDrawRect(cal_draw, &c_accent, 0, 0, CAL_W, 1);
	XftDrawRect(cal_draw, &c_accent, 0, cal_h - 1, CAL_W, 1);
	XftDrawRect(cal_draw, &c_accent, 0, 0, 1, cal_h);
	XftDrawRect(cal_draw, &c_accent, CAL_W - 1, 0, 1, cal_h);
	XClearWindow(dpy, cal);   /* the window's background is cal_buf */
}

static void cal_step(int months)
{
	cal_month += months;
	while (cal_month < 0) { cal_month += 12; cal_year--; }
	while (cal_month > 11) { cal_month -= 12; cal_year++; }
	draw_calendar();
}

static void open_calendar(void)
{
	time_t t = time(NULL);
	struct tm now;

	localtime_r(&t, &now);
	cal_year = now.tm_year + 1900;
	cal_month = now.tm_mon;
	XMoveWindow(dpy, cal, sw - CAL_W - PAD, cfg.panel_position == PANEL_TOP
	            ? panel_h : sh - panel_h - cal_h);
	XMapRaised(dpy, cal);
	for (int i = 0; i < 20; i++) {
		if (XGrabKeyboard(dpy, cal, False, GrabModeAsync, GrabModeAsync,
		                  CurrentTime) == GrabSuccess)
			break;
		usleep(10000);
	}
	XGrabPointer(dpy, cal, False, ButtonPressMask | PointerMotionMask,
	             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
	cal_open = 1;
	cal_hover = -1;
	draw_calendar();
}

static void close_calendar(void)
{
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XUnmapWindow(dpy, cal);
	cal_open = 0;
}

static int cal_arrow_at(int x, int y)
{
	for (int i = 0; i < 2; i++)
		if (x >= cal_arrow_x(i) && x < cal_arrow_x(i) + 32 &&
		    y >= CAL_MONTH_Y && y < CAL_MONTH_Y + 32)
			return i;
	return -1;
}

/* Asks Openbox to close the window, like its own x in the title bar: the
 * app gets WM_DELETE_WINDOW and can still ask "save changes?". */
static void close_window(Window w)
{
	XEvent e = { 0 };

	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = A_CLOSE;
	e.xclient.format = 32;
	e.xclient.data.l[0] = CurrentTime;
	e.xclient.data.l[1] = 2;        /* 2 = request comes from a pager/taskbar */
	XSendEvent(dpy, root, False,
	           SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

static int panel_hit(int x)
{
	if (x >= clock_x && x < clock_x + clock_w)
		return HOVER_CLOCK;
	if (update_w && x >= update_x && x < update_x + update_w)
		return HOVER_UPDATE;
	if (wifi_w && x >= wifi_x && x < wifi_x + wifi_w)
		return HOVER_WIFI;
	if (vol_w && x >= vol_x && x < vol_x + vol_w)
		return HOVER_VOLUME;
	if (bat_w && x >= bat_x && x < bat_x + bat_w)
		return HOVER_BATTERY;
	if (x >= PAD && x < PAD + START_W)
		return HOVER_START;
	for (int i = 0; i < ntasks; i++)
		if (x >= tasks[i].x && x < tasks[i].x + tasks[i].w)
			return i;
	return HOVER_NONE;
}

/* Tells Openbox where the panel is and that windows should stay above it. */
static void place_panel(void)
{
	long strut[12] = { 0 };
	XSizeHints hints = { 0 };

	panel_y = cfg.panel_position == PANEL_TOP ? 0 : sh - panel_h;
	XMoveResizeWindow(dpy, panel, 0, panel_y, sw, panel_h);

	if (cfg.panel_position == PANEL_TOP) {
		strut[2] = panel_h;      /* top */
		strut[8] = 0;            /* top_start_x */
		strut[9] = sw - 1;       /* top_end_x */
	} else {
		strut[3] = panel_h;      /* bottom */
		strut[10] = 0;           /* bottom_start_x */
		strut[11] = sw - 1;      /* bottom_end_x */
	}
	XChangeProperty(dpy, panel, A_STRUT_PARTIAL, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)strut, 12);
	XChangeProperty(dpy, panel, A_STRUT, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)strut, 4);

	hints.flags = PPosition | PSize | PMinSize | PMaxSize;
	hints.x = 0;
	hints.y = panel_y;
	hints.width = hints.min_width = hints.max_width = sw;
	hints.height = hints.min_height = hints.max_height = panel_h;
	XSetWMNormalHints(dpy, panel, &hints);

	if (draw)
		XftDrawDestroy(draw);
	if (buf)
		XFreePixmap(dpy, buf);
	buf = XCreatePixmap(dpy, panel, sw, panel_h, DefaultDepth(dpy, scr));
	draw = XftDrawCreate(dpy, buf, vis, cmap);
}

static void create_windows(void)
{
	XSetWindowAttributes a = { 0 };
	XClassHint class = { "my-panel", "my-panel" };
	long desktop_all = 0xFFFFFFFF;
	Atom states[2] = { A_STICKY, A_ABOVE };

	a.background_pixel = c_bg.pixel;
	a.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
	               LeaveWindowMask;
	panel = XCreateWindow(dpy, root, 0, 0, sw, panel_h, 0,
	                      CopyFromParent, InputOutput, CopyFromParent,
	                      CWBackPixel | CWEventMask, &a);
	XStoreName(dpy, panel, "my-panel");
	XSetClassHint(dpy, panel, &class);
	XChangeProperty(dpy, panel, A_TYPE, XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)&A_TYPE_DOCK, 1);
	XChangeProperty(dpy, panel, A_STATE, XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)states, 2);
	XChangeProperty(dpy, panel, A_DESKTOP, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)&desktop_all, 1);
	place_panel();

	/* The menu is an override-redirect window: Openbox leaves it alone. */
	a.override_redirect = True;
	a.save_under = True;
	a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
	               PointerMotionMask | KeyPressMask;
	layout_menu();
	menu = XCreateWindow(dpy, root, 0, 0, menu_w, menu_h, 0, CopyFromParent,
	                     InputOutput, CopyFromParent,
	                     CWBackPixel | CWOverrideRedirect | CWSaveUnder |
	                     CWEventMask, &a);
	size_menu_window();

	/* The calendar popup, made the same way */
	cal_h = CAL_GRID_Y + 6 * CAL_CELL + 10;
	a.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
	               KeyPressMask;
	cal = XCreateWindow(dpy, root, 0, 0, CAL_W, cal_h, 0, CopyFromParent,
	                    InputOutput, CopyFromParent,
	                    CWBackPixel | CWOverrideRedirect | CWSaveUnder |
	                    CWEventMask, &a);
	cal_buf = XCreatePixmap(dpy, cal, CAL_W, cal_h, DefaultDepth(dpy, scr));
	cal_draw = XftDrawCreate(dpy, cal_buf, vis, cmap);
	XSetWindowBackgroundPixmap(dpy, cal, cal_buf);

	XMapWindow(dpy, panel);
}

static void redraw_menu(void)
{
	draw_menu();
	XClearWindow(dpy, menu);     /* the menu's background *is* its pixmap */
}

/* --- The Windows key alone ------------------------------------------------- */

static void watch_super_key(void)
{
	int ev, err, major = 2, minor = 2;
	unsigned char bits[XIMaskLen(XI_LASTEVENT)] = { 0 };
	XIEventMask mask = { XIAllDevices, sizeof bits, bits };

	if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &err) ||
	    XIQueryVersion(dpy, &major, &minor) != Success || major * 10 + minor < 21) {
		xi_opcode = -1;           /* raw events reach us during grabs from 2.1 on */
		return;
	}
	super_l = XKeysymToKeycode(dpy, XK_Super_L);
	super_r = XKeysymToKeycode(dpy, XK_Super_R);
	XISetMask(bits, XI_RawKeyPress);
	XISetMask(bits, XI_RawKeyRelease);
	XISetMask(bits, XI_RawButtonPress);
	XISelectEvents(dpy, root, &mask, 1);
}

static int is_super(int keycode)
{
	return keycode && (keycode == super_l || keycode == super_r);
}

static void raw_event(XGenericEventCookie *c)
{
	XIRawEvent *r;

	if (c->extension != xi_opcode || !XGetEventData(dpy, c))
		return;
	r = c->data;
	/* Each key comes twice: from the keyboard itself and from the "master"
	 * keyboard that X merges all keyboards into. Count the first only. */
	if (r->deviceid == r->sourceid) {
		switch (c->evtype) {
		case XI_RawKeyPress:
			if (is_super(r->detail)) {
				if (!(r->flags & XIKeyRepeat))
					super_armed = 1;
			} else {
				super_armed = 0;      /* Super+L & co: a shortcut */
			}
			break;
		case XI_RawButtonPress:
			super_armed = 0;          /* Super + click (moving windows) */
			break;
		case XI_RawKeyRelease:
			if (is_super(r->detail) && super_armed) {
				super_armed = 0;
				/* not while the screen is locked: the menu would open
				 * behind the lock screen */
				if (XGetSelectionOwner(dpy, A_LOCK) == None)
					menu_requested = 1;
			}
			break;
		}
	}
	XFreeEventData(dpy, c);
}

static void handle(XEvent *e)
{
	KeySym key;
	int i;

	switch (e->type) {
	case GenericEvent:
		raw_event(&e->xcookie);
		break;

	case Expose:
		if (e->xexpose.window == panel && e->xexpose.count == 0)
			draw_panel();
		break;

	case ConfigureNotify:        /* screen size changed, e.g. VM window resized */
		if (e->xconfigure.window == root &&
		    (e->xconfigure.width != sw || e->xconfigure.height != sh)) {
			int old_w = menu_w, old_h = menu_h;

			sw = e->xconfigure.width;
			sh = e->xconfigure.height;
			place_panel();
			/* a smaller screen may need the menu's second column */
			if (menu_open)
				close_menu();
			layout_menu();
			if (menu_w != old_w || menu_h != old_h)
				size_menu_window();
			draw_panel();
		}
		break;

	case PropertyNotify:
		if (e->xproperty.atom == A_CLIENT_LIST || e->xproperty.atom == A_ACTIVE ||
		    e->xproperty.atom == A_NAME || e->xproperty.atom == XA_WM_NAME ||
		    e->xproperty.atom == A_STATE) {
			refresh_tasks();
			draw_panel();
		} else if (e->xproperty.atom == A_ICON) {   /* a window changed its icon */
			for (i = 0; i < ntasks; i++)
				if (tasks[i].win == e->xproperty.window)
					load_task_icon(&tasks[i]);
			draw_panel();
		}
		break;

	case MotionNotify:
		if (e->xmotion.window == panel) {
			int c;

			i = panel_hit(e->xmotion.x);
			c = on_close(i, e->xmotion.x);
			if (i != hover || c != hover_close) {
				hover = i;
				hover_close = c;
				draw_panel();
			}
		} else if (e->xmotion.window == cal) {
			i = cal_arrow_at(e->xmotion.x, e->xmotion.y);
			if (i != cal_hover) {
				cal_hover = i;
				draw_calendar();
			}
		} else if (e->xmotion.window == menu) {
			i = menu_item_at(e->xmotion.x, e->xmotion.y);
			if (i != menu_hover) {
				menu_hover = i;
				redraw_menu();
			}
		}
		break;

	case LeaveNotify:
		if (e->xcrossing.window == panel && hover != HOVER_NONE) {
			hover = HOVER_NONE;
			draw_panel();
		}
		break;

	case ButtonPress:
		if (e->xbutton.window == cal) {
			int x = e->xbutton.x, y = e->xbutton.y;

			/* wheel: change month; click outside: close */
			if (e->xbutton.button == Button4 || e->xbutton.button == Button5)
				cal_step(e->xbutton.button == Button4 ? -1 : 1);
			else if ((i = cal_arrow_at(x, y)) >= 0)
				cal_step(i == 0 ? -1 : 1);
			else if (x < 0 || y < 0 || x >= CAL_W || y >= cal_h) {
				close_calendar();
				draw_panel();
			}
			break;
		}
		if (e->xbutton.window == menu) {
			/* With the grab, every click lands here; outside means "close". */
			if (menu_item_at(e->xbutton.x, e->xbutton.y) < 0) {
				close_menu();
				draw_panel();
			}
			break;
		}
		/* The speaker: scroll = louder / quieter, click = sound on / off,
		 * right-click = all controls */
		if (panel_hit(e->xbutton.x) == HOVER_VOLUME) {
			switch (e->xbutton.button) {
			case Button4: set_volume(vol_pct + VOLUME_STEP, 1); break;
			case Button5: set_volume(vol_pct - VOLUME_STEP, -1); break;
			case Button1: set_volume(vol_pct, !vol_on); break;
			case Button3:
				run("xterm -name alsamixer -T Volume -geometry 100x26 -e alsamixer");
				break;
			}
			if (read_volume())
				draw_panel();
			break;
		}
		if (e->xbutton.button != Button1)
			break;
		i = panel_hit(e->xbutton.x);
		if (i == HOVER_CLOCK) {
			open_calendar();
		} else if (i == HOVER_UPDATE) {
			run("my-setup --update");
		} else if (i == HOVER_WIFI) {
			run("my-wifi");
		} else if (i == HOVER_START) {
			open_menu();
			redraw_menu();
		} else if (on_close(i, e->xbutton.x)) {
			close_window(tasks[i].win);
		} else if (i >= 0) {
			struct task *t = &tasks[i];

			if (t->win == active && !t->hidden)
				XIconifyWindow(dpy, t->win, scr);
			else
				activate(t->win);
		}
		draw_panel();
		break;

	case ButtonRelease:
		if (e->xbutton.window == menu) {
			i = menu_item_at(e->xbutton.x, e->xbutton.y);
			if (i >= 0) {
				close_menu();
				run(items[i].cmd);
				draw_panel();
			}
		}
		break;

	case KeyPress:
		key = XLookupKeysym(&e->xkey, 0);
		if (cal_open) {
			if (key == XK_Escape) {
				close_calendar();
				draw_panel();
			} else if (key == XK_Left || key == XK_Right) {
				cal_step(key == XK_Left ? -1 : 1);
			}
			break;
		}
		if (!menu_open)
			break;
		if (key == XK_Escape) {
			close_menu();
			draw_panel();
		} else if (key == XK_Down || key == XK_Up) {
			menu_step(key == XK_Down ? 1 : -1);
			redraw_menu();
		} else if ((key == XK_Return || key == XK_KP_Enter) && menu_hover >= 0) {
			i = menu_hover;
			close_menu();
			run(items[i].cmd);
			draw_panel();
		}
		break;
	}
}

static void on_usr1(int sig)
{
	(void)sig;
	menu_requested = 1;
}

static Atom atom(const char *name)
{
	return XInternAtom(dpy, name, False);
}

/* Settings changed: start again from the beginning with the new ones. The
 * X connection is closed on exec (FD_CLOEXEC), which removes our windows. */
static void restart(void)
{
	execv("/proc/self/exe", self_argv);
	execvp("my-panel", self_argv);
	perror("my-panel: restart");
}

static XftFont *open_font(const char *style, int size)
{
	char name[96];

	snprintf(name, sizeof name, "DejaVu Sans%s:size=%d", style, size);
	return XftFontOpenName(dpy, scr, name);
}

int main(int argc, char **argv)
{
	time_t settings_time;
	int fd;

	(void)argc;
	self_argv = argv;
	settings_load(&cfg, NULL);
	settings_time = settings_mtime(NULL);
	panel_h = cfg.panel_height;

	struct sigaction sa = { 0 };

	signal(SIGCHLD, SIG_IGN);      /* started apps are reaped automatically */
	sa.sa_handler = on_usr1;       /* no SA_RESTART: wakes up select() */
	sigaction(SIGUSR1, &sa, NULL);

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "my-panel: can't open display\n");
		return 1;
	}
	fd = ConnectionNumber(dpy);
	fcntl(fd, F_SETFD, FD_CLOEXEC); /* started apps don't inherit it */
	XSetErrorHandler(ignore_x_errors);

	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);

	A_ICON          = atom("_NET_WM_ICON");
	A_CLIENT_LIST   = atom("_NET_CLIENT_LIST");
	A_ACTIVE        = atom("_NET_ACTIVE_WINDOW");
	A_NAME          = atom("_NET_WM_NAME");
	A_UTF8          = atom("UTF8_STRING");
	A_STATE         = atom("_NET_WM_STATE");
	A_HIDDEN        = atom("_NET_WM_STATE_HIDDEN");
	A_SKIP_TASKBAR  = atom("_NET_WM_STATE_SKIP_TASKBAR");
	A_TYPE          = atom("_NET_WM_WINDOW_TYPE");
	A_TYPE_DOCK     = atom("_NET_WM_WINDOW_TYPE_DOCK");
	A_TYPE_DESKTOP  = atom("_NET_WM_WINDOW_TYPE_DESKTOP");
	A_STRUT         = atom("_NET_WM_STRUT");
	A_STRUT_PARTIAL = atom("_NET_WM_STRUT_PARTIAL");
	A_STICKY        = atom("_NET_WM_STATE_STICKY");
	A_ABOVE         = atom("_NET_WM_STATE_ABOVE");
	A_DESKTOP       = atom("_NET_WM_DESKTOP");
	A_CLOSE         = atom("_NET_CLOSE_WINDOW");
	A_LOCK          = atom("MY_LOCK_SCREEN");

	font  = open_font("", cfg.font_size);
	bold  = open_font(":bold", cfg.font_size);
	small = open_font("", cfg.font_size > 9 ? cfg.font_size - 2 : 7);
	big   = open_font(":style=ExtraLight", 30);
	if (!font || !bold || !small || !big) {
		fprintf(stderr, "my-panel: can't load font\n");
		return 1;
	}
	item_h = font->ascent + font->descent + 12;
	if (item_h < ITEM_H)
		item_h = ITEM_H;

	color(&c_bg,        cfg.pal.bg);
	color(&c_button,    cfg.pal.button);
	color(&c_hover,     cfg.pal.hover);
	color(&c_active,    cfg.pal.active);
	color(&c_text,      cfg.pal.text);
	color(&c_dim,       cfg.pal.dim);
	color(&c_accent,    cfg.pal.accent);
	color(&c_sep,       cfg.pal.sep);
	color(&c_on_accent, cfg.pal.on_accent);

	imlib_context_set_display(dpy);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cmap);
	imlib_context_set_anti_alias(1);
	imlib_set_cache_size(4 * 1024 * 1024);

	load_class_icons();
	pkg_menu_time = pkg_menu_mtime();
	load_menu();
	create_windows();

	/* Root window: window list changes and screen size changes */
	XSelectInput(dpy, root, PropertyChangeMask | StructureNotifyMask);
	watch_super_key();

	update_clock();
	refresh_tasks();
	if (open_mixer())
		restore_volume();          /* your volume from last time */
	vol_retry = time(NULL);
	read_volume();
	read_battery();
	draw_panel();

	for (;;) {
		fd_set fds;
		struct timeval tv = { 1, 0 };

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			handle(&e);
		}

		/* Sleep until an X event arrives or a second has passed (not at
		 * all when the Windows key just asked for the menu). */
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (menu_requested)
			tv.tv_sec = 0;
		select(fd + 1, &fds, NULL, NULL, &tv);

		if (menu_requested) {
			menu_requested = 0;
			if (menu_open) {
				close_menu();
			} else {
				open_menu();
				redraw_menu();
			}
			draw_panel();
		}

		if (update_clock() | read_update_status() | read_wifi() | read_volume() |
		    read_battery() || cfg.clock_seconds)
			draw_panel();
		if (cal_open)
			draw_calendar();     /* its big clock */

		/* The Software app installed or removed a program: read the
		 * menu again (it may need another column now) */
		if (pkg_menu_mtime() != pkg_menu_time && !menu_open) {
			pkg_menu_time = pkg_menu_mtime();
			load_menu();
			layout_menu();
			size_menu_window();
		}

		/* The Settings app (or you) changed the settings file */
		if (settings_mtime(NULL) != settings_time && !menu_open && !cal_open) {
			restart();
			settings_time = settings_mtime(NULL);   /* only if it failed */
		}
	}
}
