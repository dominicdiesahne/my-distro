/*
 * my-panel: the taskbar of my-distro, written with Xlib, Xft (text) and
 * Imlib2 (icons).
 *
 *   [ â˜° Start ] [ window ] [ window ] ...                    [ Mon 04.11.  14:32 ]
 *
 * - Start button opens the app menu. The entries come from /etc/my-panel/menu,
 *   one "Label|command|icon" per line, "-" for a separator line.
 *   `killall -USR1 my-panel` opens it too (Openbox binds that to Super+Space).
 * - One button per open window, with the app's icon. Click: bring it to the
 *   front; click the window that is already in front: minimise it.
 *   The icon comes from the window itself (_NET_WM_ICON, GTK apps set it) or,
 *   for apps that don't have one, from /etc/my-panel/icons (window class ->
 *   icon name in the Adwaita icon theme).
 * - Clock on the right.
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
#include <Imlib2.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define PANEL_H     36
#define PAD         4
#define START_W     100
#define TASK_MAX_W  220
#define MENU_W      240
#define HEADER_H    40
#define ITEM_H      30
#define SEP_H       11
#define MAX_TASKS   64
#define MAX_ITEMS   32
#define TITLE_MAX   200
#define ICON_SIZE   20
#define MAX_CACHE   64

#define MENU_FILE  "/etc/my-panel/menu"
#define ICONS_FILE "/etc/my-panel/icons"

enum { HOVER_NONE = -2, HOVER_START = -1 };

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
	int y, h;              /* position inside the menu window */
	Imlib_Image icon;
};

static Display *dpy;
static int scr;
static Window root, panel, menu;
static Pixmap buf, menu_buf;
static XftDraw *draw;
static Visual *vis;
static Colormap cmap;
static XftFont *font, *bold;
static int sw, sh;          /* screen size */

static XftColor c_bg, c_button, c_hover, c_active, c_text, c_dim, c_accent, c_sep;

static struct task tasks[MAX_TASKS];
static int ntasks;
static Window active;
static int hover = HOVER_NONE;

static struct item items[MAX_ITEMS];
static int nitems, menu_h, menu_open, menu_hover = -1;
static XftDraw *menu_draw;

static char clock_text[64];

/* Icons loaded from the theme, by name (NULL = not found, don't search again) */
static struct { char name[64]; Imlib_Image img; } icon_cache[MAX_CACHE];
static int nicon_cache;

/* Window class -> icon name, from /etc/my-panel/icons */
static struct { char cls[64]; char icon[64]; } class_icons[MAX_CACHE];
static int nclass_icons;
static volatile sig_atomic_t menu_requested;   /* set by SIGUSR1 */

static Atom A_ICON, A_CLIENT_LIST, A_ACTIVE, A_NAME, A_UTF8, A_STATE, A_HIDDEN,
	A_SKIP_TASKBAR, A_TYPE, A_TYPE_DOCK, A_TYPE_DESKTOP, A_STRUT,
	A_STRUT_PARTIAL, A_STICKY, A_ABOVE, A_DESKTOP;

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
		for (int i = 0; !img && i < nclass_icons; i++)
			if ((ch.res_name && strcasecmp(ch.res_name, class_icons[i].cls) == 0) ||
			    (ch.res_class && strcasecmp(ch.res_class, class_icons[i].cls) == 0))
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

static void update_clock(void)
{
	time_t now = time(NULL);

	strftime(clock_text, sizeof clock_text, "%a %d.%m.   %H:%M",
	         localtime(&now));
}

static void draw_panel(void)
{
	char label[TITLE_MAX + 8];
	int bh = PANEL_H - 2 * PAD;
	int clock_w = text_width(font, clock_text) + 24;
	int left = PAD + START_W + 2 * PAD;
	int avail = sw - left - clock_w - PAD;

	XftDrawRect(draw, &c_bg, 0, 0, sw, PANEL_H);
	XftDrawRect(draw, &c_accent, 0, 0, sw, 1);

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
			fit_text(font, t->title, w - ICON_SIZE - 26, label);
			text(draw, font, t->hidden ? &c_dim : &c_text,
			     t->x + ICON_SIZE + 16, PAD, bh, label);
		}
	}

	/* Clock */
	text(draw, font, &c_text, sw - clock_w + 12 - PAD, 0, PANEL_H, clock_text);

	XCopyArea(dpy, buf, panel, DefaultGC(dpy, scr), 0, 0, sw, PANEL_H, 0, 0);
}

static void draw_menu(void)
{
	XftDrawRect(menu_draw, &c_bg, 0, 0, MENU_W, menu_h);
	XftDrawRect(menu_draw, &c_button, 0, 0, MENU_W, HEADER_H);
	XftDrawRect(menu_draw, &c_accent, 0, HEADER_H - 2, MENU_W, 2);
	text(menu_draw, bold, &c_accent, 16, 0, HEADER_H, "my-distro");

	for (int i = 0; i < nitems; i++) {
		struct item *it = &items[i];

		if (!it->cmd[0]) {
			XftDrawRect(menu_draw, &c_sep, 12, it->y + it->h / 2, MENU_W - 24, 1);
			continue;
		}
		if (i == menu_hover)
			XftDrawRect(menu_draw, &c_hover, 0, it->y, MENU_W, it->h);
		draw_icon(menu_buf, it->icon, 16, it->y + (it->h - ICON_SIZE) / 2);
		text(menu_draw, font, &c_text, 16 + ICON_SIZE + 12, it->y, it->h,
		     it->label);
	}
	/* 1-pixel border */
	XftDrawRect(menu_draw, &c_accent, 0, 0, MENU_W, 1);
	XftDrawRect(menu_draw, &c_accent, 0, 0, 1, menu_h);
	XftDrawRect(menu_draw, &c_accent, MENU_W - 1, 0, 1, menu_h);
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

static void load_menu(void)
{
	FILE *f = fopen(MENU_FILE, "r");
	char line[400];
	int y = HEADER_H + 6;

	if (f) {
		while (fgets(line, sizeof line, f)) {
			char *bar;

			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] == '#' || line[0] == '\0')
				continue;
			if (strcmp(line, "-") == 0) {
				add_item("", "", NULL);
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
		fclose(f);
	}
	if (nitems == 0)
		add_item("Terminal", "xterm", "utilities-terminal");

	for (int i = 0; i < nitems; i++) {
		items[i].y = y;
		items[i].h = items[i].cmd[0] ? ITEM_H : SEP_H;
		y += items[i].h;
	}
	menu_h = y + 6;
}

static void run(const char *cmd)
{
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static void open_menu(void)
{
	XMoveWindow(dpy, menu, PAD, sh - PANEL_H - menu_h);
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
	if (x < 0 || x >= MENU_W)
		return -1;
	for (int i = 0; i < nitems; i++)
		if (items[i].cmd[0] && y >= items[i].y && y < items[i].y + items[i].h)
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

static int panel_hit(int x)
{
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

	XMoveResizeWindow(dpy, panel, 0, sh - PANEL_H, sw, PANEL_H);

	strut[3] = PANEL_H;          /* bottom */
	strut[10] = 0;               /* bottom_start_x */
	strut[11] = sw - 1;          /* bottom_end_x */
	XChangeProperty(dpy, panel, A_STRUT_PARTIAL, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)strut, 12);
	XChangeProperty(dpy, panel, A_STRUT, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)strut, 4);

	hints.flags = PPosition | PSize | PMinSize | PMaxSize;
	hints.x = 0;
	hints.y = sh - PANEL_H;
	hints.width = hints.min_width = hints.max_width = sw;
	hints.height = hints.min_height = hints.max_height = PANEL_H;
	XSetWMNormalHints(dpy, panel, &hints);

	if (draw)
		XftDrawDestroy(draw);
	if (buf)
		XFreePixmap(dpy, buf);
	buf = XCreatePixmap(dpy, panel, sw, PANEL_H, DefaultDepth(dpy, scr));
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
	panel = XCreateWindow(dpy, root, 0, sh - PANEL_H, sw, PANEL_H, 0,
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
	menu = XCreateWindow(dpy, root, 0, 0, MENU_W, menu_h, 0, CopyFromParent,
	                     InputOutput, CopyFromParent,
	                     CWBackPixel | CWOverrideRedirect | CWSaveUnder |
	                     CWEventMask, &a);
	menu_buf = XCreatePixmap(dpy, menu, MENU_W, menu_h, DefaultDepth(dpy, scr));
	menu_draw = XftDrawCreate(dpy, menu_buf, vis, cmap);
	XSetWindowBackgroundPixmap(dpy, menu, menu_buf);

	XMapWindow(dpy, panel);
}

static void redraw_menu(void)
{
	draw_menu();
	XClearWindow(dpy, menu);     /* the menu's background *is* its pixmap */
}

static void handle(XEvent *e)
{
	KeySym key;
	int i;

	switch (e->type) {
	case Expose:
		if (e->xexpose.window == panel && e->xexpose.count == 0)
			draw_panel();
		break;

	case ConfigureNotify:        /* screen size changed, e.g. VM window resized */
		if (e->xconfigure.window == root &&
		    (e->xconfigure.width != sw || e->xconfigure.height != sh)) {
			sw = e->xconfigure.width;
			sh = e->xconfigure.height;
			place_panel();
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
			i = panel_hit(e->xmotion.x);
			if (i != hover) {
				hover = i;
				draw_panel();
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
		if (e->xbutton.window == menu) {
			/* With the grab, every click lands here; outside means "close". */
			if (menu_item_at(e->xbutton.x, e->xbutton.y) < 0) {
				close_menu();
				draw_panel();
			}
			break;
		}
		if (e->xbutton.button != Button1)
			break;
		i = panel_hit(e->xbutton.x);
		if (i == HOVER_START) {
			open_menu();
			redraw_menu();
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

int main(void)
{
	char old_clock[64];
	int fd;

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

	font = XftFontOpenName(dpy, scr, "DejaVu Sans:size=10");
	bold = XftFontOpenName(dpy, scr, "DejaVu Sans:bold:size=10");
	if (!font || !bold) {
		fprintf(stderr, "my-panel: can't load font\n");
		return 1;
	}
	color(&c_bg,     "#0b2a2b");
	color(&c_button, "#134e4f");
	color(&c_hover,  "#1d6466");
	color(&c_active, "#2c7a7b");
	color(&c_text,   "#d8f3f0");
	color(&c_dim,    "#7fa9a6");
	color(&c_accent, "#4fd1c5");
	color(&c_sep,    "#2c5a5b");

	imlib_context_set_display(dpy);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cmap);
	imlib_context_set_anti_alias(1);
	imlib_set_cache_size(4 * 1024 * 1024);

	load_class_icons();
	load_menu();
	create_windows();

	/* Root window: window list changes and screen size changes */
	XSelectInput(dpy, root, PropertyChangeMask | StructureNotifyMask);

	update_clock();
	refresh_tasks();
	draw_panel();

	for (;;) {
		fd_set fds;
		struct timeval tv = { 1, 0 };

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			handle(&e);
		}

		/* Sleep until an X event arrives or a second has passed. */
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
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

		strcpy(old_clock, clock_text);
		update_clock();
		if (strcmp(old_clock, clock_text) != 0)
			draw_panel();
	}
}
