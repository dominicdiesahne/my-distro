/*
 * my-zoom: a screen magnifier. A window in a corner of the screen shows the
 * area around the mouse pointer, 2 to 8 times bigger.
 *
 *   my-zoom          open it; when it is open already: close it (Super+Z)
 *   my-zoom in|out   bigger / smaller (Super+plus / Super+minus)
 *
 * Every 30 ms it copies the pixels around the pointer from the screen
 * (XGetImage on the root window, which includes all windows on it) and
 * draws them scaled up with cairo. When the pointer comes near the window,
 * the window moves to the other side of the screen, so it never magnifies
 * itself. It never takes the keyboard focus.
 *
 * The running magnifier's process id is in /tmp/my-zoom.UID; "in" and "out"
 * send it SIGUSR1 / SIGUSR2.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t zoom = 2, quit;

static void on_signal(int sig)
{
	if (sig == SIGUSR1 && zoom < 8)
		zoom++;
	else if (sig == SIGUSR2 && zoom > 2)
		zoom--;
	else if (sig == SIGTERM || sig == SIGINT)
		quit = 1;
}

/* The pid of a magnifier that runs already, or 0 */
static pid_t running(const char *pidfile)
{
	FILE *f = fopen(pidfile, "r");
	int pid = 0;
	char comm[64] = "", path[64];

	if (!f)
		return 0;
	if (fscanf(f, "%d", &pid) != 1)
		pid = 0;
	fclose(f);
	/* really a my-zoom (pids are reused) */
	snprintf(path, sizeof path, "/proc/%d/comm", pid);
	if (pid > 0 && (f = fopen(path, "r"))) {
		if (!fgets(comm, sizeof comm, f))
			comm[0] = '\0';
		fclose(f);
	}
	return strncmp(comm, "my-zoom", 7) == 0 ? pid : 0;
}

int main(int argc, char **argv)
{
	char pidfile[64];
	pid_t other;
	Display *dpy;
	Window root, win;
	int scr, sw, sh, w, h, corner = 1;   /* 1: top right, 0: top left */
	Atom state, above, del;
	XWMHints hints = { .flags = InputHint, .input = False };
	XClassHint cls = { "my-zoom", "my-zoom" };
	cairo_surface_t *surf;
	cairo_t *cr;
	FILE *f;

	snprintf(pidfile, sizeof pidfile, "/tmp/my-zoom.%d", (int)getuid());
	other = running(pidfile);
	if (argc > 1 && (!strcmp(argv[1], "in") || !strcmp(argv[1], "out"))) {
		if (other)
			kill(other, argv[1][0] == 'i' ? SIGUSR1 : SIGUSR2);
		return 0;
	}
	if (other) {                       /* open: close it */
		kill(other, SIGTERM);
		return 0;
	}
	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "my-zoom: no X display\n");
		return 1;
	}
	if ((f = fopen(pidfile, "w"))) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
	signal(SIGUSR1, on_signal);
	signal(SIGUSR2, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);
	w = sw / 3;
	h = sh / 3;
	win = XCreateSimpleWindow(dpy, root, sw - w, 0, w, h, 0, 0, BlackPixel(dpy, scr));
	XStoreName(dpy, win, "Magnifier");
	XSetWMHints(dpy, win, &hints);         /* never takes the keyboard */
	XSetClassHint(dpy, win, &cls);
	/* always on top of the other windows */
	state = XInternAtom(dpy, "_NET_WM_STATE", False);
	above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
	XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&above, 1);
	del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &del, 1);
	XSelectInput(dpy, win, StructureNotifyMask | ButtonPressMask);
	XMapWindow(dpy, win);
	surf = cairo_xlib_surface_create(dpy, win, DefaultVisual(dpy, scr), w, h);
	cr = cairo_create(surf);

	while (!quit) {
		Window r, c;
		int px, py, wx, wy, x0, y0;
		unsigned int um;
		fd_set fds;
		struct timeval tv = { 0, 30000 };

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == del)
				quit = 1;
			else if (e.type == ButtonPress && e.xbutton.button == Button4 && zoom < 8)
				zoom++;
			else if (e.type == ButtonPress && e.xbutton.button == Button5 && zoom > 2)
				zoom--;
			else if (e.type == ConfigureNotify && (e.xconfigure.width != w ||
			                                       e.xconfigure.height != h)) {
				w = e.xconfigure.width;
				h = e.xconfigure.height;
				cairo_xlib_surface_set_size(surf, w, h);
			}
		}
		XQueryPointer(dpy, root, &r, &c, &px, &py, &wx, &wy, &um);

		/* Pointer near the window: move it to the other top corner */
		XTranslateCoordinates(dpy, win, root, 0, 0, &x0, &y0, &c);
		if (px > x0 - 40 && px < x0 + w + 40 && py < y0 + h + 40) {
			corner = !corner;
			XMoveWindow(dpy, win, corner ? sw - w : 0, 0);
		}

		/* The area around the pointer, w/zoom x h/zoom pixels, kept on screen */
		{
			int z = zoom, aw = w / z, ah = h / z;
			int ax = px - aw / 2, ay = py - ah / 2;
			XImage *img;

			ax = ax < 0 ? 0 : ax + aw > sw ? sw - aw : ax;
			ay = ay < 0 ? 0 : ay + ah > sh ? sh - ah : ay;
			img = aw > 0 && ah > 0 ? XGetImage(dpy, root, ax, ay, aw, ah, AllPlanes, ZPixmap) : NULL;
			if (img && img->bits_per_pixel == 32) {
				cairo_surface_t *src = cairo_image_surface_create_for_data(
					(unsigned char *)img->data, CAIRO_FORMAT_RGB24, aw, ah, img->bytes_per_line);

				cairo_save(cr);
				cairo_scale(cr, (double)w / aw, (double)h / ah);
				cairo_set_source_surface(cr, src, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
				cairo_paint(cr);
				cairo_restore(cr);
				/* where the pointer is: a small cross */
				cairo_set_source_rgb(cr, 1, 0.2, 0.2);
				cairo_set_line_width(cr, 2);
				cairo_move_to(cr, (px - ax) * (double)w / aw - 8, (py - ay) * (double)h / ah);
				cairo_rel_line_to(cr, 16, 0);
				cairo_move_to(cr, (px - ax) * (double)w / aw, (py - ay) * (double)h / ah - 8);
				cairo_rel_line_to(cr, 0, 16);
				cairo_stroke(cr);
				cairo_surface_flush(surf);
				cairo_surface_destroy(src);
			}
			if (img)
				XDestroyImage(img);
		}
		XFlush(dpy);
		FD_ZERO(&fds);
		FD_SET(ConnectionNumber(dpy), &fds);
		select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv);
	}
	unlink(pidfile);
	cairo_destroy(cr);
	cairo_surface_destroy(surf);
	XCloseDisplay(dpy);
	return 0;
}
