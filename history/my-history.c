/*
 * my-history: window for the file history (Xlib + Xft).
 *
 *   my-history          all recent changes to your files (root: everyone's),
 *                       updated live; "Undo this change" puts a file back
 *                       the way it was before the selected change
 *   my-history FILE     every saved version of FILE; "Restore" puts the
 *                       selected version back
 *
 * "View" opens the selected version in a terminal (less).
 */
#include "history.h"
#include "settings.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#define W0       860
#define H0       540
#define HEAD_H   64
#define FOOT_H   60
#define ROW_H    28
#define MAX_ROWS 1000
#define MAXV     256

struct row {
	char path[PATH_MAX];
	int version;
	time_t when;
	long long size;            /* file mode only */
};

struct button {
	const char *label;
	int x, y, w, h;
	void (*action)(void);
};

static Display *dpy;
static int scr, ww = W0, wh = H0;
static Window win;
static Pixmap buf;
static XftDraw *draw;
static Visual *vis;
static Colormap cmap;
static XftFont *font, *bold, *small;
static XftColor c_bg, c_head, c_row, c_sel, c_text, c_dim, c_accent, c_btn,
	c_btn_text, c_ok, c_err;
static Atom wm_delete;

static char file_mode[PATH_MAX];   /* empty: recent changes */
static struct row rows[MAX_ROWS];
static int nrows, sel = -1, scroll;
static char status[PATH_MAX + 128];
static int status_ok = 1;
static struct button buttons[4];
static int nbuttons;

static struct settings cfg;

static void color(XftColor *c, const char *name)
{
	XftColorAllocName(dpy, vis, cmap, name, c);
}

static int text_width(XftFont *f, const char *s)
{
	XGlyphInfo g;

	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &g);
	return g.xOff;
}

static void text(XftFont *f, XftColor *c, int x, int top, int h, const char *s)
{
	int y = top + (h - (f->ascent + f->descent)) / 2 + f->ascent;

	XftDrawStringUtf8(draw, c, f, x, y, (const FcChar8 *)s, strlen(s));
}

/* Text shortened from the left ("…/end/of/path") to fit into maxw. */
static void text_fit_left(XftFont *f, XftColor *c, int x, int top, int h,
                          const char *s, int maxw)
{
	char out[PATH_MAX + 4];
	const char *p = s;

	snprintf(out, sizeof out, "%s", s);
	while (*p && text_width(f, out) > maxw) {
		p++;
		while ((*p & 0xC0) == 0x80)
			p++;
		snprintf(out, sizeof out, "\xe2\x80\xa6%s", p);
	}
	text(f, c, x, top, h, out);
}

static void set_status(int ok, const char *msg)
{
	status_ok = ok;
	snprintf(status, sizeof status, "%s", msg);
}

static void load_rows(void)
{
	char keep_path[PATH_MAX] = "";
	int keep_version = -1;

	if (sel >= 0 && sel < nrows) {
		snprintf(keep_path, sizeof keep_path, "%s", rows[sel].path);
		keep_version = rows[sel].version;
	}
	nrows = 0;
	if (file_mode[0]) {
		int v[MAXV], n = hist_versions(file_mode, v, MAXV);

		for (int i = n - 1; i >= 0 && nrows < MAX_ROWS; i--) {
			char vf[PATH_MAX];
			struct stat st;
			struct row *r = &rows[nrows];

			hist_version_file(file_mode, v[i], vf, sizeof vf);
			if (stat(vf, &st) != 0)
				continue;
			snprintf(r->path, sizeof r->path, "%s", file_mode);
			r->version = v[i];
			r->when = st.st_mtime;
			r->size = st.st_size;
			nrows++;
		}
	} else {
		struct change *c;
		int n = hist_changes(getuid(), &c);

		for (int i = 0; i < n && nrows < MAX_ROWS; i++) {
			struct row *r = &rows[nrows++];

			snprintf(r->path, sizeof r->path, "%s", c[i].path);
			r->version = c[i].version;
			r->when = c[i].when;
			r->size = -1;
		}
		free(c);
	}
	/* keep the same row selected after a refresh */
	sel = -1;
	for (int i = 0; i < nrows; i++)
		if (rows[i].version == keep_version && strcmp(rows[i].path, keep_path) == 0)
			sel = i;
}

static int list_top(void) { return HEAD_H + ROW_H; }
static int visible_rows(void) { return (wh - list_top() - FOOT_H) / ROW_H; }

static void redraw(void)
{
	char line[PATH_MAX + 64];
	int y, vis_rows = visible_rows();

	XftDrawRect(draw, &c_bg, 0, 0, ww, wh);

	/* Header */
	XftDrawRect(draw, &c_head, 0, 0, ww, HEAD_H);
	XftDrawRect(draw, &c_accent, 0, HEAD_H - 2, ww, 2);
	if (file_mode[0]) {
		text(bold, &c_text, 20, 8, 28, "Versions of");
		text_fit_left(font, &c_dim, 20, 34, 22, file_mode, ww - 40);
	} else {
		text(bold, &c_text, 20, 8, 28, "Recent file changes");
		text(font, &c_dim, 20, 34, 22, getuid() == 0
		     ? "Every saved change on the system, newest first"
		     : "Changes to your files, newest first");
	}

	/* Column titles */
	y = HEAD_H;
	text(small, &c_dim, 20, y, ROW_H, "TIME");
	text(small, &c_dim, 200, y, ROW_H, "VERSION");
	text(small, &c_dim, 290, y, ROW_H, file_mode[0] ? "SIZE" : "FILE");

	/* Rows */
	if (nrows == 0)
		text(font, &c_dim, 20, list_top() + 10, ROW_H,
		     file_mode[0] ? "No saved versions of this file."
		                  : "No changes yet. Edit a file and it shows up here.");
	for (int i = scroll; i < nrows && i < scroll + vis_rows; i++) {
		struct row *r = &rows[i];
		int ry = list_top() + (i - scroll) * ROW_H;

		XftDrawRect(draw, i == sel ? &c_sel : (i % 2) ? &c_bg : &c_row,
		            0, ry, ww, ROW_H);
		if (i == sel)
			XftDrawRect(draw, &c_accent, 0, ry, 3, ROW_H);
		strftime(line, sizeof line, "%a %d.%m. %H:%M:%S", localtime(&r->when));
		text(font, &c_text, 20, ry, ROW_H, line);
		snprintf(line, sizeof line, r->version == 0 ? "v0 (first)" : "v%d",
		         r->version);
		text(font, &c_accent, 200, ry, ROW_H, line);
		if (file_mode[0]) {
			snprintf(line, sizeof line, "%lld bytes%s", r->size,
			         i == 0 ? "   (current)" : "");
			text(font, &c_text, 290, ry, ROW_H, line);
		} else {
			text_fit_left(font, &c_text, 290, ry, ROW_H, r->path, ww - 310);
		}
	}

	/* Footer: status + buttons */
	XftDrawRect(draw, &c_head, 0, wh - FOOT_H, ww, FOOT_H);
	{
		int bx = ww - 16;

		for (int i = nbuttons - 1; i >= 0; i--) {
			struct button *b = &buttons[i];

			b->w = text_width(bold, b->label) + 32;
			b->h = 36;
			b->x = bx - b->w;
			b->y = wh - FOOT_H + 12;
			bx = b->x - 10;
			XftDrawRect(draw, i == 0 ? &c_btn : &c_row, b->x, b->y, b->w, b->h);
			text(bold, i == 0 ? &c_btn_text : &c_text, b->x + 16, b->y, b->h,
			     b->label);
		}
		text_fit_left(small, status_ok ? &c_ok : &c_err, 20, wh - FOOT_H, FOOT_H,
		              status, bx - 30);
	}
	XCopyArea(dpy, buf, win, DefaultGC(dpy, scr), 0, 0, ww, wh, 0, 0);
}

/* --- Button actions ------------------------------------------------------ */

static void restore_to(const char *path, int v)
{
	char err[PATH_MAX + 128], msg[PATH_MAX + 64];

	if (hist_restore(path, v, err, sizeof err) != 0) {
		set_status(0, err);
		return;
	}
	snprintf(msg, sizeof msg, "Restored %s to version %d.", path, v);
	set_status(1, msg);
	usleep(300000);                       /* let my-historyd save it */
	load_rows();
}

static int need_selection(void)
{
	if (sel < 0 || sel >= nrows) {
		set_status(0, "Select a line first.");
		return 0;
	}
	return 1;
}

static void act_undo_selected(void)
{
	if (!need_selection())
		return;
	if (rows[sel].version == 0) {
		set_status(0, "That's the oldest saved version, there is nothing before it.");
		return;
	}
	restore_to(rows[sel].path, rows[sel].version - 1);
}

static void act_restore_selected(void)
{
	if (need_selection())
		restore_to(rows[sel].path, rows[sel].version);
}

static void act_undo_last(void)
{
	for (int i = 0; i < nrows; i++)
		if (rows[i].version > 0) {
			restore_to(rows[i].path, rows[i].version - 1);
			return;
		}
	set_status(0, "Nothing to undo.");
}

static void act_view(void)
{
	char vf[PATH_MAX], title[PATH_MAX + 32];

	if (!need_selection())
		return;
	hist_version_file(rows[sel].path, rows[sel].version, vf, sizeof vf);
	snprintf(title, sizeof title, "%s (version %d)", rows[sel].path,
	         rows[sel].version);
	if (fork() == 0) {
		close(ConnectionNumber(dpy));
		setsid();
		execlp("xterm", "xterm", "-name", "my-history", "-T", title,
		       "-e", "less", vf, (char *)NULL);
		_exit(127);
	}
}

static void act_refresh(void)
{
	load_rows();
	set_status(1, "");
}

static void add_button(const char *label, void (*action)(void))
{
	buttons[nbuttons++] = (struct button){ label, 0, 0, 0, 0, action };
}

/* --- Events -------------------------------------------------------------- */

static void click(int x, int y, Time t)
{
	static Time last_time;
	static int last_row = -1;

	for (int i = 0; i < nbuttons; i++) {
		struct button *b = &buttons[i];

		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
			b->action();
			return;
		}
	}
	if (y >= list_top() && y < wh - FOOT_H) {
		int r = scroll + (y - list_top()) / ROW_H;

		if (r < nrows) {
			sel = r;
			if (r == last_row && t - last_time < 400)
				act_view();               /* double click */
			last_row = r;
			last_time = t;
		}
	}
}

static void scroll_by(int d)
{
	int max = nrows - visible_rows();

	scroll += d;
	if (scroll > max)
		scroll = max;
	if (scroll < 0)
		scroll = 0;
}

static void resize(int w, int h)
{
	if (w == ww && h == wh)
		return;
	ww = w;
	wh = h;
	XftDrawDestroy(draw);
	XFreePixmap(dpy, buf);
	buf = XCreatePixmap(dpy, win, ww, wh, DefaultDepth(dpy, scr));
	draw = XftDrawCreate(dpy, buf, vis, cmap);
	scroll_by(0);
}

int main(int argc, char **argv)
{
	XSetWindowAttributes a = { 0 };
	XClassHint class = { "my-history", "my-history" };
	XSizeHints hints = { 0 };
	int fd;

	signal(SIGCHLD, SIG_IGN);
	if (argc > 1)
		hist_abspath(argv[1], file_mode, sizeof file_mode);

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "my-history: can't open display\n");
		return 1;
	}
	scr = DefaultScreen(dpy);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);

	font = XftFontOpenName(dpy, scr, "DejaVu Sans:size=10");
	bold = XftFontOpenName(dpy, scr, "DejaVu Sans:bold:size=11");
	small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=8");
	/* colors of the theme chosen in the Settings app (settings/settings.h) */
	settings_load(&cfg, NULL);
	color(&c_bg, cfg.pal.bg);
	color(&c_row, cfg.pal.button);
	color(&c_head, cfg.pal.sep);
	color(&c_sel, cfg.pal.active);
	color(&c_text, cfg.pal.text);
	color(&c_dim, cfg.pal.dim);
	color(&c_accent, cfg.pal.accent);
	color(&c_btn, cfg.pal.accent);
	color(&c_btn_text, cfg.pal.on_accent);
	color(&c_ok, cfg.pal.dark ? "#9be7c4" : "#1e7a4f");
	color(&c_err, cfg.pal.dark ? "#ff9a8f" : "#b3261e");

	if (file_mode[0]) {
		add_button("Restore this version", act_restore_selected);
		add_button("View", act_view);
		add_button("Refresh", act_refresh);
	} else {
		add_button("Undo this change", act_undo_selected);
		add_button("Undo last change", act_undo_last);
		add_button("View", act_view);
	}

	a.background_pixel = c_bg.pixel;
	a.event_mask = ExposureMask | ButtonPressMask | KeyPressMask |
	               StructureNotifyMask;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 80, 60, ww, wh, 0,
	                    CopyFromParent, InputOutput, CopyFromParent,
	                    CWBackPixel | CWEventMask, &a);
	XStoreName(dpy, win, "File history");
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", False),
	                XInternAtom(dpy, "UTF8_STRING", False), 8, PropModeReplace,
	                (unsigned char *)"File history", 12);
	XSetClassHint(dpy, win, &class);
	hints.flags = PMinSize;
	hints.min_width = 600;
	hints.min_height = 300;
	XSetWMNormalHints(dpy, win, &hints);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);

	buf = XCreatePixmap(dpy, win, ww, wh, DefaultDepth(dpy, scr));
	draw = XftDrawCreate(dpy, buf, vis, cmap);
	load_rows();
	XMapWindow(dpy, win);

	fd = ConnectionNumber(dpy);
	for (;;) {
		fd_set fds;
		struct timeval tv = { 2, 0 };

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			switch (e.type) {
			case Expose:
				if (e.xexpose.count == 0)
					redraw();
				break;
			case ConfigureNotify:
				resize(e.xconfigure.width, e.xconfigure.height);
				break;
			case ButtonPress:
				if (e.xbutton.button == Button4)
					scroll_by(-3);
				else if (e.xbutton.button == Button5)
					scroll_by(3);
				else if (e.xbutton.button == Button1)
					click(e.xbutton.x, e.xbutton.y, e.xbutton.time);
				redraw();
				break;
			case KeyPress: {
				KeySym k = XLookupKeysym(&e.xkey, 0);

				if (k == XK_Down && sel < nrows - 1)
					sel++;
				else if (k == XK_Up && sel > 0)
					sel--;
				else if (k == XK_Return)
					act_view();
				else if (k == XK_Escape)
					return 0;
				if (sel >= 0 && sel < scroll)
					scroll = sel;
				if (sel >= scroll + visible_rows())
					scroll = sel - visible_rows() + 1;
				redraw();
				break;
			}
			case ClientMessage:
				if ((Atom)e.xclient.data.l[0] == wm_delete)
					return 0;
				break;
			}
		}
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (select(fd + 1, &fds, NULL, NULL, &tv) == 0) {
			int before = nrows;

			load_rows();                  /* live update every 2 seconds */
			if (nrows != before)
				redraw();
		}
	}
}
