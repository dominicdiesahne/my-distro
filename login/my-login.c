/*
 * my-login: the graphical login screen of my-distro (Xlib, Xft, Imlib2).
 *
 * S90desktop starts the X server with this program as its only client:
 *
 *     xinit /usr/bin/my-login -- /usr/bin/Xorg :0 vt7
 *
 * It runs as root. On the first start it runs the setup (my-setup
 * --first-boot), then it loops forever:
 *   1. show the login screen (wallpaper, clock, user name + password),
 *   2. check the password against /etc/shadow with crypt(),
 *   3. start the desktop as that user: fork, setgid/initgroups/setuid,
 *      then run ~/.xinitrc (which starts Openbox),
 *   4. wait until the user logs out, stop what the session left running,
 *      and go back to 1.
 * The X server keeps running the whole time because we stay connected.
 * The "Keyboard" button at the bottom shows an on-screen keyboard (xvkbd).
 */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>
#include <Imlib2.h>
#include <crypt.h>
#include <fcntl.h>
#include <glob.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "settings.h"

#define CARD_W    380
#define CARD_H    400
#define FIELD_H   38
#define USER_MAX  32
#define PASS_MAX  128

enum { F_USER, F_PASS };

struct rect { int x, y, w, h; };

static Display *dpy;
static int scr, sw, sh;
static Window root, win;
static Visual *vis;
static Colormap cmap;
static Pixmap wall, buf;
static XftDraw *draw;
static XftFont *font, *bold, *small, *huge;
static XftColor c_text, c_dim, c_accent, c_field, c_field_on, c_button,
	c_button_text, c_error;
static Imlib_Image avatar, icon_reboot, icon_power, icon_keyboard;
static pid_t keyboard_pid;      /* the on-screen keyboard, while it is shown */

static char user[USER_MAX + 1], pass[PASS_MAX + 1], error[128];
static int focus = F_USER;
static struct rect r_user, r_pass, r_login, r_reboot, r_power, r_keyboard;
static char clock_text[32], date_text[64];

/* Colors and wallpaper: the settings of the user shown on the screen (the
 * Settings app writes them), so the login screen matches their desktop. */
static struct settings cfg;
static unsigned short bg_r, bg_g, bg_b;   /* cfg.pal.bg as numbers, for shade() */

static int ignore_x_errors(Display *d, XErrorEvent *e)
{
	(void)d; (void)e;
	return 0;
}

static void color(XftColor *c, const char *name)
{
	XftColorAllocName(dpy, vis, cmap, name, c);
}

static Imlib_Image theme_icon(const char *name)
{
	char pat[256];
	glob_t g;
	Imlib_Image img = NULL;

	snprintf(pat, sizeof pat, "/usr/share/icons/Adwaita/48x48/*/%s.png", name);
	if (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0)
		img = imlib_load_image(g.gl_pathv[0]);
	globfree(&g);
	return img;
}

static int text_width(XftFont *f, const char *s)
{
	XGlyphInfo g;

	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)s, strlen(s), &g);
	return g.xOff;
}

/* Draws s vertically centred in the box (top, h); x < 0 centres it on -x. */
static void text(XftFont *f, XftColor *c, int x, int top, int h, const char *s)
{
	int y = top + (h - (f->ascent + f->descent)) / 2 + f->ascent;

	if (x < 0)
		x = -x - text_width(f, s) / 2;
	XftDrawStringUtf8(draw, c, f, x, y, (const FcChar8 *)s, strlen(s));
}

/* Rectangle with transparency (Xft's own rectangles can't blend). */
static void shade(int x, int y, int w, int h, unsigned short r,
                  unsigned short g, unsigned short b, unsigned short a)
{
	XRenderColor c = { r * a / 255 * 257, g * a / 255 * 257,
	                   b * a / 255 * 257, a * 257 };   /* premultiplied */

	XRenderFillRectangle(dpy, PictOpOver, XftDrawPicture(draw), &c, x, y, w, h);
}

static void icon(Imlib_Image img, int x, int y, int size)
{
	if (!img)
		return;
	imlib_context_set_drawable(buf);
	imlib_context_set_image(img);
	imlib_context_set_blend(1);
	imlib_render_image_on_drawable_at_size(x, y, size, size);
}

static int inside(struct rect *r, int x, int y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/* The wallpaper, scaled to cover the whole screen, drawn once into "wall". */
static void make_wallpaper(void)
{
	char file[512];
	Imlib_Image img;

	settings_wallpaper_file(&cfg, file, sizeof file);
	img = file[0] ? imlib_load_image(file) : NULL;
	if (wall)
		XFreePixmap(dpy, wall);
	wall = XCreatePixmap(dpy, root, sw, sh, DefaultDepth(dpy, scr));
	if (!img) {
		XftDraw *d = XftDrawCreate(dpy, wall, vis, cmap);
		XftColor bg;

		color(&bg, cfg.pal.bg);
		XftDrawRect(d, &bg, 0, 0, sw, sh);
		XftDrawDestroy(d);
		return;
	}
	imlib_context_set_image(img);
	int iw = imlib_image_get_width(), ih = imlib_image_get_height();
	/* crop the image to the screen's shape, then scale */
	double scale = (double)sw / iw > (double)sh / ih ? (double)sw / iw
	                                                 : (double)sh / ih;
	int cw = sw / scale, ch = sh / scale;

	imlib_context_set_drawable(wall);
	imlib_context_set_blend(0);
	imlib_render_image_part_on_drawable_at_size((iw - cw) / 2, (ih - ch) / 2,
	                                            cw, ch, 0, 0, sw, sh);
	imlib_free_image();
}

static void field(struct rect *r, const char *label, const char *value,
                  int secret, int focused)
{
	char shown[PASS_MAX * 3 + 1] = "";
	int maxw = r->w - 24;

	text(small, &c_dim, r->x, r->y - 24, 20, label);
	XftDrawRect(draw, focused ? &c_field_on : &c_field, r->x, r->y, r->w, r->h);
	XftDrawRect(draw, focused ? &c_accent : &c_dim, r->x, r->y + r->h - 2,
	            r->w, 2);
	if (secret) {
		for (size_t i = 0; i < strlen(value); i++)
			strcat(shown, "\xe2\x80\xa2");          /* "•" */
		/* too many dots? show only the last ones */
		while (shown[0] && text_width(font, shown) > maxw)
			memmove(shown, shown + 3, strlen(shown + 3) + 1);
	} else {
		snprintf(shown, sizeof shown, "%s", value);
	}
	text(font, &c_text, r->x + 12, r->y, r->h, shown);
	if (focused) {                                  /* text cursor */
		int cx = r->x + 12 + text_width(font, shown) + 1;
		XftDrawRect(draw, &c_accent, cx, r->y + 9, 2, r->h - 18);
	}
}

static void redraw(void)
{
	int cx = sw / 2;
	int x = cx - CARD_W / 2;
	int y = sh / 2 - CARD_H / 2 + 40;
	char host[64] = "my-distro";

	XCopyArea(dpy, wall, buf, DefaultGC(dpy, scr), 0, 0, sw, sh, 0, 0);

	/* Clock above the card */
	text(huge, &c_text, -cx, y - 170, 90, clock_text);
	text(font, &c_dim, -cx, y - 80, 30, date_text);

	/* The card */
	shade(x, y, CARD_W, CARD_H, bg_r, bg_g, bg_b, 220);
	XftDrawRect(draw, &c_accent, x, y, CARD_W, 2);

	icon(avatar, cx - 32, y + 26, 64);
	text(bold, &c_text, -cx, y + 96, 30, "Welcome");

	r_user = (struct rect){ x + 30, y + 160, CARD_W - 60, FIELD_H };
	r_pass = (struct rect){ x + 30, y + 236, CARD_W - 60, FIELD_H };
	r_login = (struct rect){ x + 30, y + 296, CARD_W - 60, 42 };
	field(&r_user, "User", user, 0, focus == F_USER);
	field(&r_pass, "Password", pass, 1, focus == F_PASS);

	XftDrawRect(draw, &c_button, r_login.x, r_login.y, r_login.w, r_login.h);
	text(bold, &c_button_text, -cx, r_login.y, r_login.h, "Log in");

	if (error[0])
		text(small, &c_error, -cx, y + 352, 30, error);

	/* Bottom bar: host name left, reboot / power off right */
	shade(0, sh - 48, sw, 48, bg_r, bg_g, bg_b, 180);
	gethostname(host, sizeof host - 1);
	text(font, &c_dim, 20, sh - 48, 48, host);

	r_power = (struct rect){ sw - 140, sh - 44, 124, 40 };
	r_reboot = (struct rect){ sw - 264, sh - 44, 116, 40 };
	icon(icon_reboot, r_reboot.x + 8, r_reboot.y + 10, 20);
	text(font, &c_text, r_reboot.x + 36, r_reboot.y, r_reboot.h, "Reboot");
	icon(icon_power, r_power.x + 8, r_power.y + 10, 20);
	text(font, &c_text, r_power.x + 36, r_power.y, r_power.h, "Power off");
	r_keyboard = (struct rect){ sw - 400, sh - 44, 128, 40 };
	icon(icon_keyboard, r_keyboard.x + 8, r_keyboard.y + 10, 20);
	text(font, keyboard_pid ? &c_accent : &c_text, r_keyboard.x + 36, r_keyboard.y,
	     r_keyboard.h, "Keyboard");

	XCopyArea(dpy, buf, win, DefaultGC(dpy, scr), 0, 0, sw, sh, 0, 0);
	XFlush(dpy);
}

static int update_clock(void)
{
	char old[32];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	strcpy(old, clock_text);
	struct settings c = cfg;

	c.clock_seconds = 0;
	settings_format_time(&c, tm, clock_text, sizeof clock_text);
	strftime(date_text, sizeof date_text, "%A, %d %B %Y", tm);
	return strcmp(old, clock_text) != 0;
}

/* Reads the settings of the user in the user field and sets the colors. */
static void load_theme(void)
{
	struct passwd *pw = user[0] ? getpwnam(user) : NULL;
	unsigned r = 0, g = 0, b = 0;

	settings_load(&cfg, pw ? pw->pw_dir : "/root");
	sscanf(cfg.pal.bg + 1, "%2x%2x%2x", &r, &g, &b);
	bg_r = r; bg_g = g; bg_b = b;
	color(&c_text, cfg.pal.text);
	color(&c_dim, cfg.pal.dim);
	color(&c_accent, cfg.pal.accent);
	color(&c_field, cfg.pal.button);
	color(&c_field_on, cfg.pal.hover);
	color(&c_button, cfg.pal.accent);
	color(&c_button_text, cfg.pal.on_accent);
	color(&c_error, cfg.pal.dark ? "#ff9a8f" : "#b3261e");
	make_wallpaper();
	XSetWindowBackgroundPixmap(dpy, root, wall);   /* also behind the session */
	XClearWindow(dpy, root);
	update_clock();
}

/*
 * First start: before the first login screen, my-setup asks for the keyboard,
 * your own account and the computer's name. It writes /etc/my-distro/setup-done
 * at the end, so it runs only once (on the live ISO: once per boot).
 */
static void first_boot_setup(void)
{
	pid_t pid;

	if (access("/etc/my-distro/setup-done", F_OK) == 0)
		return;
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XSync(dpy, False);
	pid = fork();
	if (pid == 0) {
		clearenv();
		setenv("HOME", "/root", 1);
		setenv("DISPLAY", ":0", 1);
		setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
		setenv("LANG", "en_US.UTF-8", 1);
		setenv("NO_AT_BRIDGE", "1", 1);
		execl("/usr/bin/my-setup", "my-setup", "--first-boot", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

/* The first normal user (uid 1000 and up) is filled in for convenience. */
static void default_user(void)
{
	struct passwd *pw;

	setpwent();
	while ((pw = getpwent()))
		if (pw->pw_uid >= 1000 && pw->pw_uid < 60000) {
			snprintf(user, sizeof user, "%s", pw->pw_name);
			focus = F_PASS;
			break;
		}
	endpwent();
}

/*
 * Password check. /etc/shadow holds a hash like "$5$salt$hash" (SHA-256).
 * crypt() hashes what was typed with the same salt; same result = correct.
 */
static struct passwd *check_password(void)
{
	struct passwd *pw = getpwnam(user);
	struct spwd *sp;
	const char *hash;
	char *typed;

	if (!pw)
		return NULL;
	hash = pw->pw_passwd;
	sp = getspnam(user);
	if (sp)
		hash = sp->sp_pwdp;
	if (!hash || hash[0] == '!' || hash[0] == '*')
		return NULL;                     /* locked account */
	if (hash[0] == '\0')
		return pw;                       /* no password set */
	typed = crypt(pass, hash);
	return typed && strcmp(typed, hash) == 0 ? pw : NULL;
}

/*
 * The on-screen keyboard (xvkbd): its keys are typed into the login screen
 * like real ones (XTEST). It runs as the user "nobody", not as root like this
 * program: xvkbd can run commands from its menus.
 */
static void stop_keyboard(void)
{
	if (keyboard_pid > 0) {
		kill(keyboard_pid, SIGTERM);
		waitpid(keyboard_pid, NULL, 0);
	}
	keyboard_pid = 0;
}

static void toggle_keyboard(void)
{
	char geo[64];
	int kw = sw * 2 / 3, kh = kw / 3;
	pid_t pid;

	if (keyboard_pid) {
		stop_keyboard();
		return;
	}
	snprintf(geo, sizeof geo, "%dx%d+%d+%d", kw, kh, (sw - kw) / 2, sh - kh - 60);
	pid = fork();
	if (pid == 0) {
		struct passwd *nobody = getpwnam("nobody");

		setsid();
		if (!nobody || setgroups(0, NULL) != 0 || setgid(nobody->pw_gid) != 0 ||
		    setuid(nobody->pw_uid) != 0)
			_exit(1);
		clearenv();
		setenv("DISPLAY", ":0", 1);
		setenv("HOME", "/tmp", 1);
		setenv("PATH", "/usr/bin:/bin", 1);
		execl("/usr/bin/xvkbd", "xvkbd", "-compact", "-no-keypad", "-geometry", geo,
		      (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		keyboard_pid = pid;
}

static void grab_keyboard(void)
{
	for (int i = 0; i < 50; i++) {
		if (XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync,
		                  CurrentTime) == GrabSuccess)
			return;
		usleep(20000);
	}
}

static void show(void)
{
	XMapRaised(dpy, win);
	XSync(dpy, False);
	grab_keyboard();
	XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
	redraw();
}

static void hide(void)
{
	XUngrabKeyboard(dpy, CurrentTime);
	XUnmapWindow(dpy, win);
	XSync(dpy, False);
}

/* Runs the user's desktop and returns when they log out. */
static void run_session(struct passwd *pw)
{
	pid_t pid;
	uid_t uid = pw->pw_uid;
	gid_t gid = pw->pw_gid;
	char name[USER_MAX + 1], home[256], shell[256];

	/* getpwnam's buffer is reused; keep copies */
	snprintf(name, sizeof name, "%s", pw->pw_name);
	snprintf(home, sizeof home, "%s", pw->pw_dir);
	snprintf(shell, sizeof shell, "%s", pw->pw_shell);

	stop_keyboard();          /* the desktop has its own (Start menu) */
	hide();
	pid = fork();
	if (pid == 0) {
		setsid();
		/* Drop root: groups first, then the group, then the user. */
		if (initgroups(name, gid) != 0 || setgid(gid) != 0 || setuid(uid) != 0)
			_exit(1);
		if (chdir(home) != 0 && chdir("/") != 0)
			_exit(1);
		clearenv();
		setenv("HOME", home, 1);
		setenv("USER", name, 1);
		setenv("LOGNAME", name, 1);
		setenv("SHELL", shell, 1);
		setenv("DISPLAY", ":0", 1);
		setenv("LANG", "en_US.UTF-8", 1);
		setenv("PATH", uid == 0 ? "/sbin:/bin:/usr/sbin:/usr/bin"
		                        : "/usr/local/bin:/usr/bin:/bin", 1);
		execl("/bin/sh", "sh", "-c",
		      "if [ -x \"$HOME/.xinitrc\" ]; then exec \"$HOME/.xinitrc\"; "
		      "else exec openbox; fi", (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);

	/* The session's leftovers (taskbar, desktop icons, open apps): first
	 * everything in its process group, then everything the user still runs. */
	if (pid > 0)
		kill(-pid, SIGTERM);
	if (uid != 0) {
		pid_t killer = fork();

		if (killer == 0) {
			if (setuid(uid) == 0) {
				kill(-1, SIGTERM);   /* as that user: only hits their processes */
				sleep(1);
				kill(-1, SIGKILL);
			}
			_exit(0);
		}
		if (killer > 0)
			waitpid(killer, NULL, 0);
	}
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
}

static void submit(void)
{
	struct passwd *pw;

	if (!user[0]) {
		focus = F_USER;
		return;
	}
	pw = check_password();
	memset(pass, 0, sizeof pass);           /* don't keep it around */
	if (!pw) {
		sleep(1);                            /* slows down guessing */
		snprintf(error, sizeof error, "Wrong user name or password");
		focus = F_PASS;
		return;
	}
	error[0] = '\0';
	run_session(pw);
	default_user();
	load_theme();         /* they may have changed it in the session */
	show();
}

static void type_into(char *dst, size_t max, const char *s)
{
	size_t len = strlen(dst);

	for (; *s; s++)
		if ((unsigned char)*s >= 0x20 && *s != 0x7f && len < max)
			dst[len++] = *s;
	dst[len] = '\0';
}

static void key(XKeyEvent *e)
{
	char typed[32];
	KeySym ks;
	int n = XLookupString(e, typed, sizeof typed - 1, &ks, NULL);
	char *dst = focus == F_USER ? user : pass;
	size_t max = focus == F_USER ? USER_MAX : PASS_MAX;

	typed[n > 0 ? n : 0] = '\0';
	error[0] = '\0';
	switch (ks) {
	case XK_Tab: case XK_Down: case XK_Up: case XK_ISO_Left_Tab:
		focus = focus == F_USER ? F_PASS : F_USER;
		break;
	case XK_Return: case XK_KP_Enter:
		if (focus == F_USER)
			focus = F_PASS;
		else
			submit();
		break;
	case XK_BackSpace:
		if (dst[0])
			dst[strlen(dst) - 1] = '\0';
		break;
	case XK_Escape:
		dst[0] = '\0';
		break;
	default:
		/* Latin-1 from XLookupString: keep plain ASCII only */
		for (int i = 0; i < n; i++)
			if ((unsigned char)typed[i] >= 0x80)
				typed[i] = '\0';
		type_into(dst, max, typed);
	}
	redraw();
}

static void click(int x, int y)
{
	if (inside(&r_user, x, y))
		focus = F_USER;
	else if (inside(&r_pass, x, y))
		focus = F_PASS;
	else if (inside(&r_login, x, y))
		submit();
	else if (inside(&r_reboot, x, y))
		kill(1, SIGTERM);                    /* our init: TERM = reboot */
	else if (inside(&r_power, x, y))
		kill(1, SIGUSR2);                    /* our init: USR2 = power off */
	else if (inside(&r_keyboard, x, y))
		toggle_keyboard();
	redraw();
}

int main(void)
{
	XSetWindowAttributes a = { 0 };
	int fd;

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "my-login: can't open display\n");
		return 1;
	}
	fd = ConnectionNumber(dpy);
	fcntl(fd, F_SETFD, FD_CLOEXEC);          /* the session doesn't inherit it */
	XSetErrorHandler(ignore_x_errors);

	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);

	imlib_context_set_display(dpy);
	imlib_context_set_visual(vis);
	imlib_context_set_colormap(cmap);
	imlib_context_set_anti_alias(1);

	font  = XftFontOpenName(dpy, scr, "DejaVu Sans:size=11");
	bold  = XftFontOpenName(dpy, scr, "DejaVu Sans:bold:size=13");
	small = XftFontOpenName(dpy, scr, "DejaVu Sans:size=9");
	huge  = XftFontOpenName(dpy, scr, "DejaVu Sans:style=ExtraLight:size=56");

	avatar = theme_icon("avatar-default");
	if (!avatar)
		avatar = theme_icon("system-users");
	icon_reboot = theme_icon("view-refresh");
	icon_power = theme_icon("system-shutdown");
	icon_keyboard = theme_icon("input-keyboard");

	buf = XCreatePixmap(dpy, root, sw, sh, DefaultDepth(dpy, scr));
	draw = XftDrawCreate(dpy, buf, vis, cmap);

	/* Full-screen window; no window manager runs yet, so nobody moves it. */
	a.override_redirect = True;
	a.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;
	win = XCreateWindow(dpy, root, 0, 0, sw, sh, 0, CopyFromParent,
	                    InputOutput, CopyFromParent,
	                    CWOverrideRedirect | CWEventMask, &a);
	XDefineCursor(dpy, win, XCreateFontCursor(dpy, XC_left_ptr));
	XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));

	first_boot_setup();
	default_user();
	load_theme();
	show();

	for (;;) {
		fd_set fds;
		struct timeval tv = { 1, 0 };

		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			if (e.type == Expose && e.xexpose.count == 0)
				redraw();
			else if (e.type == KeyPress)
				key(&e.xkey);
			else if (e.type == ButtonPress)
				click(e.xbutton.x, e.xbutton.y);
		}
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		select(fd + 1, &fds, NULL, NULL, &tv);
		if (update_clock())
			redraw();
	}
}
