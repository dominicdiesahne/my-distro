/*
 * my-lock: the screen lock of my-distro (Xlib, Xft, Imlib2).
 *
 *     my-lock               lock the screen now (Super+L, Start menu)
 *     my-lock --background  lock, and return as soon as the screen is locked
 *                           (used before suspending: my-suspend, my-lidd)
 *     my-lock --watch       runs through the whole session (Openbox's autostart)
 *                           and locks after "lock_minutes" without a key press
 *                           or mouse move (Settings app, default 10)
 *
 * The lock is one window over the whole screen with the wallpaper, the time
 * and a password field. It takes the keyboard and the mouse for itself (an X
 * "grab"), so no other program gets any input until it is unlocked. Every
 * half second it puts itself on top again, in case something tries to
 * appear above it. The password is checked by my-checkpass (setuid root,
 * because only root can read /etc/shadow).
 *
 * Only one lock runs at a time: the lock window owns the X selection
 * MY_LOCK_SCREEN; a second my-lock sees that and does nothing.
 *
 * Idle time comes from the X server's screen saver extension (libXss). A
 * video player that asks for no screen saver (mpv does) stops the lock.
 */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/scrnsaver.h>
#include <Imlib2.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "settings.h"
#include "i18n.h"

#define PASS_MAX   128
#define CARD_W     380
#define FIELD_H    40
#define LOCK_ATOM  "MY_LOCK_SCREEN"

static Display *dpy;
static int scr, sw, sh;
static Window root, win;
static Visual *vis;
static Colormap cmap;
static Pixmap wall, buf;
static XftDraw *draw;
static XftFont *font, *bold, *small, *huge;
static XftColor c_text, c_dim, c_accent, c_field, c_error;
static struct settings cfg;
static unsigned short bg_r, bg_g, bg_b;

static char pass[PASS_MAX + 1], message[128], clock_text[32], date_text[64];
static char who[128];
static int checking;

static int ignore_x_errors(Display *d, XErrorEvent *e)
{
	(void)d; (void)e;
	return 0;
}

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

/* Text centred on x, vertically centred in (top, h) */
static void text_c(XftFont *f, XftColor *c, int x, int top, int h, const char *s)
{
	int y = top + (h - (f->ascent + f->descent)) / 2 + f->ascent;

	XftDrawStringUtf8(draw, c, f, x - text_width(f, s) / 2, y,
	                  (const FcChar8 *)s, strlen(s));
}

static void shade(int x, int y, int w, int h, unsigned short a)
{
	XRenderColor c = { bg_r * a / 255 * 257, bg_g * a / 255 * 257,
	                   bg_b * a / 255 * 257, a * 257 };

	XRenderFillRectangle(dpy, PictOpOver, XftDrawPicture(draw), &c, x, y, w, h);
}

/* The wallpaper, covering the screen, darkened a little */
static void make_wallpaper(void)
{
	char file[512];
	Imlib_Image img;

	wall = XCreatePixmap(dpy, root, sw, sh, DefaultDepth(dpy, scr));
	settings_wallpaper_file(&cfg, file, sizeof file);
	img = file[0] ? imlib_load_image(file) : NULL;
	if (img) {
		imlib_context_set_image(img);
		int iw = imlib_image_get_width(), ih = imlib_image_get_height();
		double scale = (double)sw / iw > (double)sh / ih ? (double)sw / iw
		                                                 : (double)sh / ih;
		int cw = sw / scale, ch = sh / scale;

		imlib_context_set_drawable(wall);
		imlib_context_set_blend(0);
		imlib_render_image_part_on_drawable_at_size((iw - cw) / 2, (ih - ch) / 2,
		                                            cw, ch, 0, 0, sw, sh);
		imlib_free_image();
	} else {
		XftDraw *d = XftDrawCreate(dpy, wall, vis, cmap);
		XftColor bg;

		color(&bg, cfg.pal.bg);
		XftDrawRect(d, &bg, 0, 0, sw, sh);
		XftDrawDestroy(d);
	}
}

static int update_clock(void)
{
	char old[32];
	time_t now = time(NULL);
	struct tm tm;
	struct settings c = cfg;

	tzset();
	localtime_r(&now, &tm);
	snprintf(old, sizeof old, "%s", clock_text);
	c.clock_seconds = 0;
	settings_format_time(&c, &tm, clock_text, sizeof clock_text);
	/* TRANSLATORS: the date on the login screen, see "man strftime" */
	strftime(date_text, sizeof date_text, _("%A, %d %B %Y"), &tm);
	return strcmp(old, clock_text) != 0;
}

/* "Switch user" below the card: someone else logs in on a second screen
 * while this desktop stays locked (my-power switch-user) */
static int sw_x, sw_y, sw_w, sw_h;

static void switch_user(void)
{
	pid_t pid = fork();

	if (pid == 0) {
		execl("/usr/bin/my-power", "my-power", "switch-user", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

static void redraw(void)
{
	int cx = sw / 2, y = sh / 2 - 60, fx = cx - (CARD_W - 60) / 2;
	char dots[PASS_MAX * 3 + 1] = "";

	XCopyArea(dpy, wall, buf, DefaultGC(dpy, scr), 0, 0, sw, sh, 0, 0);
	shade(0, 0, sw, sh, 90);

	text_c(huge, &c_text, cx, y - 200, 100, clock_text);
	text_c(font, &c_dim, cx, y - 100, 30, date_text);

	shade(cx - CARD_W / 2, y, CARD_W, 200, 220);
	XftDrawRect(draw, &c_accent, cx - CARD_W / 2, y, CARD_W, 2);
	text_c(bold, &c_text, cx, y + 20, 34, _("Locked"));
	text_c(small, &c_dim, cx, y + 54, 22, who);

	XftDrawRect(draw, &c_field, fx, y + 92, CARD_W - 60, FIELD_H);
	XftDrawRect(draw, &c_accent, fx, y + 92 + FIELD_H - 2, CARD_W - 60, 2);
	for (size_t i = 0; i < strlen(pass); i++)
		strcat(dots, "\xe2\x80\xa2");                        /* "•" */
	while (dots[0] && text_width(font, dots) > CARD_W - 84)
		memmove(dots, dots + 3, strlen(dots + 3) + 1);
	if (dots[0])
		text_c(font, &c_text, cx, y + 92, FIELD_H, dots);
	else
		text_c(small, &c_dim, cx, y + 92, FIELD_H,
		       checking ? "" : _("Type your password and press Enter"));

	if (checking)
		text_c(small, &c_dim, cx, y + 150, 30, _("Checking…"));
	else if (message[0])
		text_c(small, &c_error, cx, y + 150, 30, message);

	sw_w = text_width(font, _("Switch user")) + 40;
	sw_h = 38;
	sw_x = cx - sw_w / 2;
	sw_y = y + 200 + 24;
	shade(sw_x, sw_y, sw_w, sw_h, 200);
	text_c(font, &c_text, cx, sw_y, sw_h, _("Switch user"));

	XCopyArea(dpy, buf, win, DefaultGC(dpy, scr), 0, 0, sw, sh, 0, 0);
	XFlush(dpy);
}

/* Runs my-checkpass with the password on its standard input. */
static int password_ok(void)
{
	int fd[2], st = 0;
	pid_t pid;

	if (pipe(fd) != 0)
		return 0;
	pid = fork();
	if (pid == 0) {
		dup2(fd[0], 0);
		close(fd[0]);
		close(fd[1]);
		execl("/usr/bin/my-checkpass", "my-checkpass", (char *)NULL);
		_exit(1);
	}
	close(fd[0]);
	if (write(fd[1], pass, strlen(pass)) < 0 || write(fd[1], "\n", 1) < 0) {
		/* nothing: my-checkpass then sees a wrong password */
	}
	close(fd[1]);
	if (pid < 0 || waitpid(pid, &st, 0) < 0)
		return 0;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int grab(void)
{
	int kb = 0, ptr = 0;

	/* A menu that is open (the taskbar's) or a key still held down by a
	 * shortcut can hold a grab for a moment: keep trying for 3 seconds. */
	for (int i = 0; i < 150 && !(kb && ptr); i++) {
		if (!kb)
			kb = XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync,
			                   CurrentTime) == GrabSuccess;
		if (!ptr)
			ptr = XGrabPointer(dpy, win, False, ButtonPressMask, GrabModeAsync,
			                   GrabModeAsync, win, None, CurrentTime) == GrabSuccess;
		if (!(kb && ptr))
			usleep(20000);
	}
	return kb && ptr;
}

static int key(XKeyEvent *e)
{
	char typed[32];
	KeySym ks;
	int n = XLookupString(e, typed, sizeof typed - 1, &ks, NULL);
	size_t len = strlen(pass);

	message[0] = '\0';
	switch (ks) {
	case XK_Return: case XK_KP_Enter:
		checking = 1;
		redraw();
		if (password_ok()) {
			memset(pass, 0, sizeof pass);
			return 1;                        /* unlocked */
		}
		checking = 0;
		memset(pass, 0, sizeof pass);
		snprintf(message, sizeof message, "%s", _("Wrong password"));
		break;
	case XK_BackSpace:
		if (len)
			pass[len - 1] = '\0';
		break;
	case XK_Escape:
		pass[0] = '\0';
		break;
	default:
		/* plain ASCII only, like the login screen */
		for (int i = 0; i < n && len < PASS_MAX; i++)
			if ((unsigned char)typed[i] >= 0x20 && (unsigned char)typed[i] < 0x7f)
				pass[len++] = typed[i];
		pass[len] = '\0';
	}
	redraw();
	return 0;
}

static int locked_already(void)
{
	return XGetSelectionOwner(dpy, XInternAtom(dpy, LOCK_ATOM, False)) != None;
}

/*
 * Locks the screen and returns when it is unlocked. ready_fd >= 0: write a
 * byte to it once the screen is locked (--background waits for that).
 */
static int lock(int ready_fd)
{
	XSetWindowAttributes a = { 0 };
	struct passwd *pw = getpwuid(getuid());
	unsigned r = 0, g = 0, b = 0;
	int fd = ConnectionNumber(dpy);

	settings_load(&cfg, NULL);
	sscanf(cfg.pal.bg + 1, "%2x%2x%2x", &r, &g, &b);
	bg_r = r; bg_g = g; bg_b = b;
	color(&c_text, cfg.pal.text);
	color(&c_dim, cfg.pal.dim);
	color(&c_accent, cfg.pal.accent);
	color(&c_field, cfg.pal.button);
	color(&c_error, cfg.pal.dark ? "#ff9a8f" : "#b3261e");
	if (pw)
		snprintf(who, sizeof who, "%s", pw->pw_gecos && pw->pw_gecos[0]
		         ? pw->pw_gecos : pw->pw_name);
	*strchrnul(who, ',') = '\0';             /* "Full Name,room,phone" */

	font  = XftFontOpenName(dpy, scr, "DejaVu Sans:size=12");
	bold  = XftFontOpenName(dpy, scr, "DejaVu Sans:bold:size=14");
	small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=10");
	huge  = XftFontOpenName(dpy, scr, "DejaVu Sans:style=ExtraLight:size=64");
	make_wallpaper();
	buf = XCreatePixmap(dpy, root, sw, sh, DefaultDepth(dpy, scr));
	draw = XftDrawCreate(dpy, buf, vis, cmap);
	update_clock();

	a.override_redirect = True;
	a.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | VisibilityChangeMask;
	win = XCreateWindow(dpy, root, 0, 0, sw, sh, 0, CopyFromParent, InputOutput,
	                    CopyFromParent, CWOverrideRedirect | CWEventMask, &a);
	XStoreName(dpy, win, "my-lock");
	XDefineCursor(dpy, win, XCreateFontCursor(dpy, XC_left_ptr));
	XMapRaised(dpy, win);
	XSync(dpy, False);
	if (!grab()) {
		fprintf(stderr, "my-lock: could not take the keyboard and mouse\n");
		return 1;
	}
	XSetSelectionOwner(dpy, XInternAtom(dpy, LOCK_ATOM, False), win, CurrentTime);
	redraw();
	XSync(dpy, False);
	if (ready_fd >= 0) {
		if (write(ready_fd, "L", 1) < 0) {
			/* the parent is gone: nothing to tell */
		}
		close(ready_fd);
	}

	for (;;) {
		fd_set fds;
		struct timeval tv = { 0, 500000 };

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			if (e.type == Expose && e.xexpose.count == 0)
				redraw();
			else if (e.type == KeyPress && key(&e.xkey))
				return 0;
			else if (e.type == ButtonPress && e.xbutton.x >= sw_x &&
			         e.xbutton.x < sw_x + sw_w && e.xbutton.y >= sw_y &&
			         e.xbutton.y < sw_y + sw_h)
				switch_user();
			else if (e.type == VisibilityNotify &&
			         e.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
		}
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		select(fd + 1, &fds, NULL, NULL, &tv);
		XRaiseWindow(dpy, win);           /* stay on top of everything */
		if (update_clock())
			redraw();
	}
}

/* --watch: lock after lock_minutes without input */
static int watch(char *self)
{
	int ev, err;
	XScreenSaverInfo *info;

	if (!XScreenSaverQueryExtension(dpy, &ev, &err)) {
		fprintf(stderr, "my-lock: the X server has no screen saver extension\n");
		return 1;
	}
	info = XScreenSaverAllocInfo();
	for (;;) {
		sleep(5);
		/* collect locks that ended (not SIG_IGN: the locks would inherit
		 * it, see main) */
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		settings_load(&cfg, NULL);            /* the setting may have changed */
		if (cfg.lock_minutes <= 0 || !XScreenSaverQueryInfo(dpy, root, info))
			continue;
		/* a video player asked for no screen saver: don't lock either */
		if (info->state == ScreenSaverDisabled)
			continue;
		if (info->idle >= (unsigned long)cfg.lock_minutes * 60000 && !locked_already()) {
			if (fork() == 0) {
				execl(self, "my-lock", (char *)NULL);
				execlp("my-lock", "my-lock", (char *)NULL);
				_exit(127);
			}
			sleep(10);                        /* let it start */
		}
	}
}

int main(int argc, char **argv)
{
	int background = argc > 1 && strcmp(argv[1], "--background") == 0;
	int fd[2] = { -1, -1 };

	/* Started by a program that ignores SIGCHLD (the taskbar does, so its
	 * apps are reaped by themselves), we would inherit that, and waitpid()
	 * for my-checkpass would fail: every password would look wrong. */
	signal(SIGCHLD, SIG_DFL);
	i18n_init();

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "my-lock: can't open display\n");
		return 1;
	}
	XSetErrorHandler(ignore_x_errors);
	fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);   /* not for the locks --watch starts */
	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);

	if (argc > 1 && strcmp(argv[1], "--watch") == 0)
		return watch("/proc/self/exe");
	if (argc > 1 && !background) {
		fprintf(stderr, "usage: my-lock [--background | --watch]\n");
		return 2;
	}
	if (locked_already())
		return 0;

	imlib_context_set_display(dpy);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cmap);
	imlib_context_set_anti_alias(1);

	if (background) {
		/* The child locks; we wait until it says the screen is locked (or
		 * failed), so whoever started us can suspend right after. */
		char c;
		pid_t pid;

		if (pipe(fd) != 0)
			return 1;
		XCloseDisplay(dpy);                   /* the child opens its own */
		pid = fork();
		if (pid < 0)
			return 1;
		if (pid > 0) {
			close(fd[1]);
			return read(fd[0], &c, 1) == 1 ? 0 : 1;
		}
		close(fd[0]);
		setsid();
		dpy = XOpenDisplay(NULL);
		if (!dpy)
			return 1;
		XSetErrorHandler(ignore_x_errors);
		root = RootWindow(dpy, scr);
		vis = DefaultVisual(dpy, scr);
		cmap = DefaultColormap(dpy, scr);
		imlib_context_set_display(dpy);
		imlib_context_set_visual(vis);
		imlib_context_set_colormap(cmap);
	}
	return lock(fd[1]);
}
