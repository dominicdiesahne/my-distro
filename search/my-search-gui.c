/*
 * my-search-gui: the "Search files" window (Xlib + Xft).
 *
 * Type what you're looking for; results appear while you type (after a
 * short pause), best match first, with the part of the file that matched.
 * Enter / double-click opens the file (text files in the editor), and the
 * buttons open its folder or its version history.
 */
#define _GNU_SOURCE
#include "search.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define W0       780
#define H0       580
#define HEAD_H   104
#define FOOT_H   60
#define ITEM_H   70
#define RESULTS  30
#define QUERY_MAX 400
#define DELAY_MS 350          /* search this long after the last key press */

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
static XftFont *font, *bold, *small, *big;
static XftColor c_bg, c_head, c_box, c_sel, c_text, c_dim, c_accent, c_btn,
	c_btn_text, c_err;
static Atom wm_delete;

static char query[QUERY_MAX + 1], searched[QUERY_MAX + 1], message[256];
static struct index ix;
static time_t ix_mtime;
static int model_ready;
static float *qvec;
static struct hit hits[RESULTS];
static int nhits, sel = -1, scroll;
static long long search_at;           /* ms timestamp; 0 = nothing pending */
static struct button buttons[3];

static long long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static void color(XftColor *c, const char *name)
{
	XftColorAllocName(dpy, vis, cmap, name, c);
}

static int text_width(XftFont *f, const char *s, int len)
{
	XGlyphInfo g;

	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, len, &g);
	return g.xOff;
}

/* Draws s in one line, cut with "…" to fit into maxw. Newlines -> spaces. */
static void text(XftFont *f, XftColor *c, int x, int top, int h,
                 const char *s, int maxw)
{
	char line[1024];
	int len = 0, y = top + (h - (f->ascent + f->descent)) / 2 + f->ascent;

	for (; *s && len < (int)sizeof line - 4; s++)
		line[len++] = (*s == '\n' || *s == '\t' || *s == '\r') ? ' ' : *s;
	line[len] = '\0';
	if (maxw > 0 && text_width(f, line, len) > maxw) {
		while (len > 0 && text_width(f, line, len) + 12 > maxw) {
			len--;
			while (len > 0 && ((unsigned char)line[len] & 0xC0) == 0x80)
				len--;
		}
		memcpy(line + len, "\xe2\x80\xa6", 4);
		len += 3;
	}
	XftDrawStringUtf8(draw, c, f, x, y, (const FcChar8 *)line, len);
}

static void read_status(char *out, size_t n)
{
	char file[4096];
	FILE *f;

	search_file("status", file, sizeof file);
	f = fopen(file, "r");
	if (!f || !fgets(out, n, f))
		snprintf(out, n, "Starting the indexer\xe2\x80\xa6");
	out[strcspn(out, "\n")] = '\0';
	if (f)
		fclose(f);
}

static void redraw(void)
{
	char line[256];
	int list_h = wh - HEAD_H - FOOT_H, cx;

	XftDrawRect(draw, &c_bg, 0, 0, ww, wh);

	/* Search box */
	XftDrawRect(draw, &c_head, 0, 0, ww, HEAD_H);
	XftDrawRect(draw, &c_box, 20, 20, ww - 40, 46);
	XftDrawRect(draw, &c_accent, 20, 64, ww - 40, 2);
	if (query[0])
		text(big, &c_text, 34, 20, 46, query, ww - 80);
	else
		text(big, &c_dim, 34, 20, 46,
		     "Search your files by meaning\xe2\x80\xa6", ww - 80);
	cx = 34 + (query[0] ? text_width(big, query, strlen(query)) : 0) + 1;
	if (cx < ww - 40)
		XftDrawRect(draw, &c_accent, cx, 30, 2, 26);
	read_status(line, sizeof line);
	text(small, &c_dim, 22, 72, 26, line, ww - 44);

	/* Results */
	if (message[0])
		text(font, &c_err, 24, HEAD_H + 16, 30, message, ww - 48);
	else if (searched[0] && nhits == 0)
		text(font, &c_dim, 24, HEAD_H + 16, 30, "Nothing found.", 0);
	else if (!searched[0])
		text(font, &c_dim, 24, HEAD_H + 16, 30,
		     "Try something like \"notes about my training plan\" or \"recipe\".", ww - 48);
	for (int i = scroll; i < nhits && (i - scroll) * ITEM_H < list_h; i++) {
		const struct chunk *c = &ix.c[hits[i].chunk];
		int y = HEAD_H + (i - scroll) * ITEM_H;
		const char *name = strrchr(c->path, '/');
		int pct = (int)(hits[i].score * 100 + 0.5);

		if (i == sel) {
			XftDrawRect(draw, &c_sel, 0, y, ww, ITEM_H);
			XftDrawRect(draw, &c_accent, 0, y, 3, ITEM_H);
		}
		snprintf(line, sizeof line, "%d%%", pct < 0 ? 0 : pct);
		text(bold, &c_accent, ww - 70, y + 6, 22, line, 0);
		/* match bar */
		XftDrawRect(draw, &c_box, ww - 120, y + 32, 96, 4);
		XftDrawRect(draw, &c_accent, ww - 120, y + 32,
		            96 * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100, 4);
		text(bold, &c_text, 24, y + 6, 22, name ? name + 1 : c->path, ww - 170);
		text(small, &c_dim, 24, y + 27, 16, c->path, ww - 170);
		text(font, &c_text, 24, y + 44, 22,
		     c->text[0] ? c->text : "(no text, matched by its name)", ww - 48);
	}

	/* Buttons */
	XftDrawRect(draw, &c_head, 0, wh - FOOT_H, ww, FOOT_H);
	{
		int bx = ww - 16;

		for (int i = 2; i >= 0; i--) {
			struct button *b = &buttons[i];

			b->w = text_width(bold, b->label, strlen(b->label)) + 32;
			b->h = 36;
			b->x = bx - b->w;
			b->y = wh - FOOT_H + 12;
			bx = b->x - 10;
			XftDrawRect(draw, i == 0 ? &c_btn : &c_box, b->x, b->y, b->w, b->h);
			text(bold, i == 0 ? &c_btn_text : &c_text, b->x + 16, b->y, b->h,
			     b->label, 0);
		}
		snprintf(line, sizeof line, nhits ? "%d files found" : "", nhits);
		text(small, &c_dim, 20, wh - FOOT_H, FOOT_H, line, 0);
	}
	XCopyArea(dpy, buf, win, DefaultGC(dpy, scr), 0, 0, ww, wh, 0, 0);
}

/* --- Searching ----------------------------------------------------------- */

static void reload_index_if_changed(void)
{
	char file[4096];
	struct stat st;

	search_file("index", file, sizeof file);
	if (stat(file, &st) != 0 || st.st_mtime == ix_mtime)
		return;
	index_free(&ix);
	index_load(&ix, file, embed_dim());
	ix_mtime = st.st_mtime;
}

static void run_search(void)
{
	search_at = 0;
	message[0] = '\0';
	snprintf(searched, sizeof searched, "%s", query);
	nhits = 0;
	sel = -1;
	scroll = 0;
	if (!query[0])
		return;
	if (!model_ready) {
		if (embed_init(MODEL_PATH) != 0) {
			snprintf(message, sizeof message, "Can't load the search model.");
			return;
		}
		qvec = malloc(embed_dim() * sizeof(float));
		model_ready = 1;
	}
	reload_index_if_changed();
	if (ix.n == 0) {
		snprintf(message, sizeof message,
		         "The index is still empty, the indexer is working on it.");
		return;
	}
	if (embed_text(query, qvec) != 0) {
		snprintf(message, sizeof message, "Couldn't process that query.");
		return;
	}
	nhits = index_search(&ix, qvec, query, hits, RESULTS);
	sel = nhits > 0 ? 0 : -1;
}

/* --- Actions ------------------------------------------------------------- */

static void spawn(char *const argv[])
{
	if (fork() == 0) {
		close(ConnectionNumber(dpy));
		setsid();
		execvp(argv[0], argv);
		_exit(127);
	}
}

static const struct chunk *selected(void)
{
	return sel >= 0 && sel < nhits ? &ix.c[hits[sel].chunk] : NULL;
}

static void act_open(void)
{
	const struct chunk *c = selected();
	char path[4096], title[4096];

	if (!c)
		return;
	snprintf(path, sizeof path, "%s", c->path);
	if (c->text[0]) {                        /* text file: the editor */
		snprintf(title, sizeof title, "%s", basename(path));
		char *argv[] = { "xterm", "-name", "editor", "-T", title, "-e", "nano",
		                 (char *)c->path, NULL };
		spawn(argv);
	} else {                                 /* anything else: its folder */
		char *argv[] = { "pcmanfm", dirname(path), NULL };
		spawn(argv);
	}
}

static void act_folder(void)
{
	const struct chunk *c = selected();
	char path[4096];

	if (!c)
		return;
	snprintf(path, sizeof path, "%s", c->path);
	char *argv[] = { "pcmanfm", dirname(path), NULL };
	spawn(argv);
}

static void act_history(void)
{
	const struct chunk *c = selected();

	if (!c)
		return;
	char *argv[] = { "my-history", (char *)c->path, NULL };
	spawn(argv);
}

/* --- Events -------------------------------------------------------------- */

static void type_text(const char *s, int n)
{
	size_t len = strlen(query);

	for (int i = 0; i < n; i++) {
		unsigned char ch = s[i];

		if (ch < 0x20 || ch == 0x7f)
			continue;
		if (ch >= 0x80) {                    /* Latin-1 (ä, ö, ü) -> UTF-8 */
			if (ch < 0xa0 || len + 2 > QUERY_MAX)
				continue;
			query[len++] = 0xc0 | (ch >> 6);
			query[len++] = 0x80 | (ch & 0x3f);
		} else if (len < QUERY_MAX) {
			query[len++] = ch;
		}
	}
	query[len] = '\0';
}

static void ensure_visible(void)
{
	int rows = (wh - HEAD_H - FOOT_H) / ITEM_H;

	if (sel >= 0 && sel < scroll)
		scroll = sel;
	if (rows > 0 && sel >= scroll + rows)
		scroll = sel - rows + 1;
}

static int key(XKeyEvent *e)
{
	char s[32];
	KeySym k;
	int n = XLookupString(e, s, sizeof s, &k, NULL);
	size_t len = strlen(query);

	switch (k) {
	case XK_Escape:
		if (!query[0])
			return 1;                        /* quit */
		query[0] = '\0';
		run_search();
		break;
	case XK_Return: case XK_KP_Enter:
		if (strcmp(query, searched) != 0)
			run_search();
		else
			act_open();
		break;
	case XK_Down:
		if (sel < nhits - 1)
			sel++;
		ensure_visible();
		break;
	case XK_Up:
		if (sel > 0)
			sel--;
		ensure_visible();
		break;
	case XK_BackSpace:
		while (len > 0 && ((unsigned char)query[len - 1] & 0xC0) == 0x80)
			len--;
		if (len > 0)
			len--;
		query[len] = '\0';
		search_at = now_ms() + DELAY_MS;
		break;
	default:
		if (n > 0) {
			type_text(s, n);
			search_at = now_ms() + DELAY_MS;
		}
	}
	return 0;
}

static void click(int x, int y, Time t)
{
	static Time last_t;
	static int last_item = -1;

	for (int i = 0; i < 3; i++) {
		struct button *b = &buttons[i];

		if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
			b->action();
			return;
		}
	}
	if (y >= HEAD_H && y < wh - FOOT_H) {
		int item = scroll + (y - HEAD_H) / ITEM_H;

		if (item < nhits) {
			sel = item;
			if (item == last_item && t - last_t < 400)
				act_open();
			last_item = item;
			last_t = t;
		}
	}
}

int main(void)
{
	XSetWindowAttributes a = { 0 };
	XClassHint class = { "my-search-gui", "my-search-gui" };
	XSizeHints hints = { 0 };
	int fd;

	signal(SIGCHLD, SIG_IGN);
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "my-search-gui: can't open display\n");
		return 1;
	}
	scr = DefaultScreen(dpy);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);

	font = XftFontOpenName(dpy, scr, "DejaVu Sans:size=10");
	bold = XftFontOpenName(dpy, scr, "DejaVu Sans:bold:size=11");
	small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=8");
	big = XftFontOpenName(dpy, scr, "DejaVu Sans:size=14");
	color(&c_bg, "#0b2a2b");
	color(&c_head, "#071f20");
	color(&c_box, "#123f40");
	color(&c_sel, "#1a5a5b");
	color(&c_text, "#d8f3f0");
	color(&c_dim, "#7fa9a6");
	color(&c_accent, "#4fd1c5");
	color(&c_btn, "#4fd1c5");
	color(&c_btn_text, "#062022");
	color(&c_err, "#ff9a8f");

	buttons[0] = (struct button){ "Open", 0, 0, 0, 0, act_open };
	buttons[1] = (struct button){ "Open folder", 0, 0, 0, 0, act_folder };
	buttons[2] = (struct button){ "History", 0, 0, 0, 0, act_history };

	a.background_pixel = c_bg.pixel;
	a.event_mask = ExposureMask | ButtonPressMask | KeyPressMask |
	               StructureNotifyMask;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 120, 60, ww, wh, 0,
	                    CopyFromParent, InputOutput, CopyFromParent,
	                    CWBackPixel | CWEventMask, &a);
	XStoreName(dpy, win, "Search files");
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", False),
	                XInternAtom(dpy, "UTF8_STRING", False), 8, PropModeReplace,
	                (unsigned char *)"Search files", 12);
	XSetClassHint(dpy, win, &class);
	hints.flags = PMinSize;
	hints.min_width = 500;
	hints.min_height = 320;
	XSetWMNormalHints(dpy, win, &hints);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);

	buf = XCreatePixmap(dpy, win, ww, wh, DefaultDepth(dpy, scr));
	draw = XftDrawCreate(dpy, buf, vis, cmap);
	XMapWindow(dpy, win);

	fd = ConnectionNumber(dpy);
	for (;;) {
		fd_set fds;
		long long wait = 1000;
		struct timeval tv;

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			switch (e.type) {
			case Expose:
				if (e.xexpose.count == 0)
					redraw();
				break;
			case ConfigureNotify:
				if (e.xconfigure.width != ww || e.xconfigure.height != wh) {
					ww = e.xconfigure.width;
					wh = e.xconfigure.height;
					XftDrawDestroy(draw);
					XFreePixmap(dpy, buf);
					buf = XCreatePixmap(dpy, win, ww, wh, DefaultDepth(dpy, scr));
					draw = XftDrawCreate(dpy, buf, vis, cmap);
				}
				break;
			case ButtonPress:
				if (e.xbutton.button == Button4 && scroll > 0)
					scroll--;
				else if (e.xbutton.button == Button5 && scroll < nhits - 1)
					scroll++;
				else if (e.xbutton.button == Button1)
					click(e.xbutton.x, e.xbutton.y, e.xbutton.time);
				redraw();
				break;
			case KeyPress:
				if (key(&e.xkey))
					return 0;
				redraw();
				break;
			case ClientMessage:
				if ((Atom)e.xclient.data.l[0] == wm_delete)
					return 0;
				break;
			}
		}
		if (search_at) {
			wait = search_at - now_ms();
			if (wait <= 0) {
				run_search();
				redraw();
				continue;
			}
		}
		tv.tv_sec = wait / 1000;
		tv.tv_usec = (wait % 1000) * 1000;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		if (select(fd + 1, &fds, NULL, NULL, &tv) == 0 && !search_at)
			redraw();                         /* refresh the indexer status */
	}
}
