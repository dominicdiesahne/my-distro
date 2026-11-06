/*
 * my-screenshot: saves a picture of the screen as a PNG file.
 *
 *     my-screenshot            the whole screen               (Print)
 *     my-screenshot --window   the window in front            (Alt+Print)
 *     my-screenshot --area     an area you drag with the mouse (Shift+Print)
 *     my-screenshot --delay N  wait N seconds first (with any of the above)
 *     my-screenshot ... FILE   save as FILE instead
 *
 * The picture goes to ~/Pictures/Screenshot YYYY-MM-DD HH-MM-SS.png and a
 * notification says where. Openbox binds the keys (desktop/post-build.sh).
 *
 * How: Xlib gives us the screen's pixels (imlib2 copies them out of the root
 * window, windows on top included) and imlib2 writes the PNG. For --area we
 * grab the mouse, draw the rectangle with XOR straight onto the screen (drawing
 * it a second time removes it again), and take the picture after letting go.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <Imlib2.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static Display *dpy;
static Window root;

static void notify(const char *urgency, const char *summary, const char *body)
{
	pid_t pid = fork();

	if (pid == 0) {
		execlp("my-notify", "my-notify", "-u", urgency, "-i", "camera-photo",
		       summary, body, (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* The window in front, with Openbox's frame around it (title bar included):
 * the frame is the window's ancestor right below the root window. */
static int active_window(int *x, int *y, int *w, int *h)
{
	Atom type, a = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	Window win, parent, rt, *kids;
	unsigned int nkids, bw, depth, uw, uh;

	if (XGetWindowProperty(dpy, root, a, 0, 1, False, XA_WINDOW, &type, &format,
	                       &n, &after, &data) != Success || !data || n == 0)
		return 0;
	win = *(Window *)data;
	XFree(data);
	if (win == None)
		return 0;
	for (;;) {
		if (!XQueryTree(dpy, win, &rt, &parent, &kids, &nkids))
			return 0;
		if (kids)
			XFree(kids);
		if (parent == root || parent == None)
			break;
		win = parent;
	}
	if (!XGetGeometry(dpy, win, &rt, x, y, &uw, &uh, &bw, &depth))
		return 0;
	*w = uw + 2 * bw;
	*h = uh + 2 * bw;
	return 1;
}

static void xor_rect(GC gc, int x0, int y0, int x1, int y1)
{
	int x = x0 < x1 ? x0 : x1, y = y0 < y1 ? y0 : y1;
	int w = abs(x1 - x0), h = abs(y1 - y0);

	if (w > 0 && h > 0)
		XDrawRectangle(dpy, root, gc, x, y, w, h);
}

/* Drag a rectangle with the mouse. Esc or a right-click cancels (returns 0). */
static int pick_area(int *x, int *y, int *w, int *h)
{
	Cursor cross = XCreateFontCursor(dpy, XC_crosshair);
	XGCValues gv = { 0 };
	GC gc;
	XEvent e;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0, down = 0, result = -1;

	gv.function = GXxor;
	gv.foreground = WhitePixel(dpy, DefaultScreen(dpy)) ^ BlackPixel(dpy, DefaultScreen(dpy));
	gv.line_width = 1;
	gv.subwindow_mode = IncludeInferiors;   /* draw over the windows too */
	gc = XCreateGC(dpy, root, GCFunction | GCForeground | GCLineWidth | GCSubwindowMode, &gv);

	/* Right after a keyboard shortcut Openbox may still hold the keyboard */
	for (int i = 0; i < 50; i++) {
		if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask |
		                 PointerMotionMask, GrabModeAsync, GrabModeAsync, None,
		                 cross, CurrentTime) == GrabSuccess)
			break;
		usleep(20000);
	}
	for (int i = 0; i < 50; i++) {
		if (XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync,
		                  CurrentTime) == GrabSuccess)
			break;
		usleep(20000);
	}

	while (result < 0) {
		XNextEvent(dpy, &e);
		switch (e.type) {
		case ButtonPress:
			if (e.xbutton.button != Button1) {
				result = 0;
				break;
			}
			x0 = x1 = e.xbutton.x_root;
			y0 = y1 = e.xbutton.y_root;
			down = 1;
			break;
		case MotionNotify:
			if (!down)
				break;
			while (XCheckTypedEvent(dpy, MotionNotify, &e))
				;                       /* only the newest position */
			xor_rect(gc, x0, y0, x1, y1);   /* away with the old one */
			x1 = e.xmotion.x_root;
			y1 = e.xmotion.y_root;
			xor_rect(gc, x0, y0, x1, y1);
			break;
		case ButtonRelease:
			if (!down)
				break;
			xor_rect(gc, x0, y0, x1, y1);
			x1 = e.xbutton.x_root;
			y1 = e.xbutton.y_root;
			result = 1;
			break;
		case KeyPress:
			if (XLookupKeysym(&e.xkey, 0) == XK_Escape) {
				if (down)
					xor_rect(gc, x0, y0, x1, y1);
				result = 0;
			}
			break;
		}
	}
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XFreeGC(dpy, gc);
	XFreeCursor(dpy, cross);
	XSync(dpy, False);

	*x = x0 < x1 ? x0 : x1;
	*y = y0 < y1 ? y0 : y1;
	*w = abs(x1 - x0) + 1;
	*h = abs(y1 - y0) + 1;
	if (result == 1 && (*w < 3 || *h < 3)) {
		/* just a click: the window under the mouse would be nice, but the
		 * whole screen is what people expect from "nothing chosen" */
		*x = *y = 0;
		*w = DisplayWidth(dpy, DefaultScreen(dpy));
		*h = DisplayHeight(dpy, DefaultScreen(dpy));
	}
	return result;
}

/* ~/Pictures/Screenshot 2026-11-06 14-32-05.png (Pictures is made if needed) */
static void default_name(char *out, size_t len)
{
	const char *home = getenv("HOME");
	char dir[400], stamp[64];
	time_t t = time(NULL);
	struct tm tm;

	snprintf(dir, sizeof dir, "%s/Pictures", home ? home : "/tmp");
	mkdir(dir, 0755);
	localtime_r(&t, &tm);
	strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &tm);
	snprintf(out, len, "%s/Screenshot %s.png", dir, stamp);
	/* two in the same second: add a number */
	for (int i = 2; access(out, F_OK) == 0 && i < 100; i++)
		snprintf(out, len, "%s/Screenshot %s (%d).png", dir, stamp, i);
}

int main(int argc, char **argv)
{
	enum { SCREEN, WINDOW, AREA } what = SCREEN;
	const char *file = NULL;
	char path[512], msg[600];
	int delay = 0, x = 0, y = 0, w, h, sw, sh;
	Imlib_Image img;
	Imlib_Load_Error err;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--window") == 0 || strcmp(argv[i], "-w") == 0)
			what = WINDOW;
		else if (strcmp(argv[i], "--area") == 0 || strcmp(argv[i], "-a") == 0)
			what = AREA;
		else if ((strcmp(argv[i], "--delay") == 0 || strcmp(argv[i], "-d") == 0) &&
		         i + 1 < argc)
			delay = atoi(argv[++i]);
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: my-screenshot [--window | --area] [--delay N] [FILE]\n");
			return 2;
		} else
			file = argv[i];
	}

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "my-screenshot: can't open display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);
	sw = w = DisplayWidth(dpy, DefaultScreen(dpy));
	sh = h = DisplayHeight(dpy, DefaultScreen(dpy));

	if (what == AREA && pick_area(&x, &y, &w, &h) == 0)
		return 0;                           /* cancelled */
	if (delay > 0)
		sleep(delay);
	if (what == WINDOW && !active_window(&x, &y, &w, &h)) {
		x = y = 0;                          /* no window: the whole screen */
		w = sw;
		h = sh;
	}
	/* parts outside the screen can't be copied */
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > sw) w = sw - x;
	if (y + h > sh) h = sh - y;
	if (w <= 0 || h <= 0)
		return 1;
	if (what == AREA)
		usleep(150000);                     /* the screen without our rectangle */

	imlib_context_set_display(dpy);
	imlib_context_set_visual(DefaultVisual(dpy, DefaultScreen(dpy)));
	imlib_context_set_colormap(DefaultColormap(dpy, DefaultScreen(dpy)));
	imlib_context_set_drawable(root);
	img = imlib_create_image_from_drawable(0, x, y, w, h, 1);
	if (!img) {
		notify("critical", "Screenshot failed", "Could not read the screen.");
		return 1;
	}
	imlib_context_set_image(img);
	imlib_image_set_format("png");
	if (file)
		snprintf(path, sizeof path, "%s", file);
	else
		default_name(path, sizeof path);
	imlib_save_image_with_error_return(path, &err);
	imlib_free_image();
	if (err != IMLIB_LOAD_ERROR_NONE) {
		snprintf(msg, sizeof msg, "Could not write %s", path);
		notify("critical", "Screenshot failed", msg);
		fprintf(stderr, "my-screenshot: %s\n", msg);
		return 1;
	}
	printf("%s\n", path);
	snprintf(msg, sizeof msg, "%dx%d, saved as %s", w, h, path);
	notify("normal", "Screenshot saved", msg);
	return 0;
}
