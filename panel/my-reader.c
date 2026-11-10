/*
 * my-reader: a small screen reader for my-distro (0.9.7). Super+Alt+S, or
 * Settings, Accessibility, switches it on and off.
 *
 * It speaks (eSpeak, like my-speak):
 *   - the title of the window that comes to the front, for every program
 *     (the window manager names it in _NET_ACTIVE_WINDOW)
 *   - what gets the keyboard focus in GTK programs: buttons, check boxes
 *     (and whether they are ticked), text fields (and their text, except
 *     passwords), sliders (their value), list rows, tabs, menu items. GTK
 *     programs tell the accessibility bus (AT-SPI, at-spi2-core) about it;
 *     we listen there. That covers the file manager, the web browser's
 *     window (not the web page itself) and all of my-distro's own windows.
 *   - the letter you type into a text field
 *   - the highlighted entry of the Start menu (my-panel puts it into the
 *     root window's _MY_READER_TEXT property; the taskbar is not GTK)
 * It doesn't read: terminals (xterm), web page contents in NetSurf, the
 * login and lock screens, windows that run as root (Users and system,
 * Disks), and it has no "read the whole window" or review commands like
 * Orca. Something new interrupts what is being said.
 *
 *   my-reader             run (in the background of the session)
 *   my-reader --toggle    on/off (Super+Alt+S); remembered in the settings
 *   my-reader --autostart run if the settings say so (at login)
 *   my-reader --stop      switch it off
 *
 * The voice follows the session's language (eSpeak has all of
 * my-distro's); MY_SPEAK_VOICE=... picks another.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <atspi/atspi.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "settings.h"
#include "i18n.h"

static Display *dpy;
static Window root;
static Atom a_active, a_name, a_utf8, a_reader;
static Window last_active;
static pid_t speaker;            /* the running espeak | aplay, its own group */
static char voice[32] = "en";

/* --- Speaking ------------------------------------------------------------------------ */

static void hush(void)
{
	if (speaker > 0) {
		kill(-speaker, SIGTERM);
		waitpid(speaker, NULL, 0);
	}
	speaker = 0;
}

/* Says text, interrupting what is being said */
static void say(const char *text)
{
	pid_t pid;

	if (!text || !text[strspn(text, " \t\r\n")])
		return;
	/* MY_READER_LOG=FILE: also write it down (for tests, and to see why
	 * something sounds wrong) */
	if (getenv("MY_READER_LOG")) {
		FILE *log = fopen(getenv("MY_READER_LOG"), "a");

		if (log) {
			fprintf(log, "%s\n", text);
			fclose(log);
		}
	}
	hush();
	pid = fork();
	if (pid == 0) {
		FILE *p;
		char cmd[128];

		signal(SIGCHLD, SIG_DFL);         /* pclose waits for its own child */
		signal(SIGTERM, SIG_DFL);
		setsid();
		snprintf(cmd, sizeof cmd, "espeak --stdin --stdout -s 170 -v %s | aplay -q", voice);
		if ((p = popen(cmd, "w"))) {
			fputs(text, p);
			fputc('\n', p);
			pclose(p);
		}
		_exit(0);
	}
	speaker = pid;
}

static void reap(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

/* --- What GTK programs tell AT-SPI ------------------------------------------------------ */

static int has_state(AtspiAccessible *a, AtspiStateType s)
{
	AtspiStateSet *set = atspi_accessible_get_state_set(a);
	int r = set && atspi_state_set_contains(set, s);

	if (set)
		g_object_unref(set);
	return r;
}

/* "OK, push button", "Dark theme, check box, ticked", "Name, text, Anna" */
static void describe(AtspiAccessible *a)
{
	AtspiRole role = atspi_accessible_get_role(a, NULL);
	char *name = atspi_accessible_get_name(a, NULL);
	char *rname = atspi_accessible_get_localized_role_name(a, NULL);
	GString *s = g_string_new(NULL);

	if (name && *name)
		g_string_append_printf(s, "%s, ", name);
	if (rname)
		g_string_append(s, rname);
	switch (role) {
	case ATSPI_ROLE_CHECK_BOX:
	case ATSPI_ROLE_CHECK_MENU_ITEM:
	case ATSPI_ROLE_RADIO_BUTTON:
	case ATSPI_ROLE_RADIO_MENU_ITEM:
	case ATSPI_ROLE_TOGGLE_BUTTON:
		g_string_append_printf(s, ", %s", has_state(a, ATSPI_STATE_CHECKED) ?
		                       _("ticked") : _("not ticked"));
		break;
	case ATSPI_ROLE_SLIDER:
	case ATSPI_ROLE_SPIN_BUTTON:
	case ATSPI_ROLE_SCROLL_BAR: {
		AtspiValue *v = atspi_accessible_get_value_iface(a);

		if (v) {
			g_string_append_printf(s, ", %g", atspi_value_get_current_value(v, NULL));
			g_object_unref(v);
		}
		break;
	}
	case ATSPI_ROLE_PASSWORD_TEXT:
		break;                    /* never read a password */
	case ATSPI_ROLE_ENTRY:
	case ATSPI_ROLE_TEXT: {
		AtspiText *t = atspi_accessible_get_text_iface(a);

		if (t) {
			char *txt = atspi_text_get_text(t, 0, 200, NULL);

			if (txt && *txt)
				g_string_append_printf(s, ", %s", txt);
			g_free(txt);
			g_object_unref(t);
		}
		break;
	}
	default:
		break;
	}
	if (!has_state(a, ATSPI_STATE_ENABLED) && role != ATSPI_ROLE_LABEL)
		g_string_append_printf(s, ", %s", _("unavailable"));
	say(s->str);
	g_string_free(s, TRUE);
	g_free(name);
	g_free(rname);
}

static void on_focus(AtspiEvent *e, void *d)
{
	(void)d;
	if (e->detail1 && e->source)
		describe(e->source);
}

/* Menu items, list rows and tabs get "selected" rather than the focus */
static void on_selected(AtspiEvent *e, void *d)
{
	AtspiRole r;

	(void)d;
	if (!e->detail1 || !e->source)
		return;
	r = atspi_accessible_get_role(e->source, NULL);
	if (r == ATSPI_ROLE_MENU_ITEM || r == ATSPI_ROLE_CHECK_MENU_ITEM ||
	    r == ATSPI_ROLE_RADIO_MENU_ITEM || r == ATSPI_ROLE_MENU ||
	    r == ATSPI_ROLE_LIST_ITEM || r == ATSPI_ROLE_TABLE_CELL || r == ATSPI_ROLE_PAGE_TAB)
		describe(e->source);
}

/* Typing into a text field: the letter (not in password fields) */
static void on_insert(AtspiEvent *e, void *d)
{
	AtspiRole r;
	const char *t;

	(void)d;
	if (!e->source || !has_state(e->source, ATSPI_STATE_FOCUSED))
		return;
	r = atspi_accessible_get_role(e->source, NULL);
	if (r == ATSPI_ROLE_PASSWORD_TEXT)
		return;
	if (G_VALUE_HOLDS_STRING(&e->any_data) && (t = g_value_get_string(&e->any_data)) &&
	    g_utf8_strlen(t, -1) <= 2)
		say(strcmp(t, " ") == 0 ? _("space") : t);
}

/* --- Window titles and the Start menu (X11) -------------------------------------------- */

static char *x_text(Window w, Atom prop)
{
	Atom type;
	int format;
	unsigned long n, left;
	unsigned char *data = NULL;
	char *r = NULL;

	if (XGetWindowProperty(dpy, w, prop, 0, 1024, False, AnyPropertyType, &type, &format,
	                       &n, &left, &data) == Success && data && format == 8)
		r = g_strndup((char *)data, n);
	if (data)
		XFree(data);
	return r;
}

static void active_window_changed(void)
{
	Atom type;
	int format;
	unsigned long n, left;
	unsigned char *data = NULL;
	Window w = None;

	if (XGetWindowProperty(dpy, root, a_active, 0, 1, False, XA_WINDOW, &type, &format,
	                       &n, &left, &data) == Success && data && n == 1)
		w = *(Window *)data;
	if (data)
		XFree(data);
	if (w == None || w == last_active)
		return;
	last_active = w;
	{
		char *title = x_text(w, a_name);

		if (!title) {
			XTextProperty tp;

			if (XGetWMName(dpy, w, &tp) && tp.value) {
				title = g_strndup((char *)tp.value, tp.nitems);
				XFree(tp.value);
			}
		}
		say(title);
		g_free(title);
	}
}

static gboolean on_x(GIOChannel *ch, GIOCondition c, gpointer d)
{
	(void)ch; (void)c; (void)d;
	while (XPending(dpy)) {
		XEvent e;

		XNextEvent(dpy, &e);
		if (e.type != PropertyNotify || e.xproperty.window != root)
			continue;
		if (e.xproperty.atom == a_active) {
			active_window_changed();
		} else if (e.xproperty.atom == a_reader) {
			char *t = x_text(root, a_reader);

			say(t);
			g_free(t);
		}
	}
	return G_SOURCE_CONTINUE;
}

/* --- On, off, remembered ---------------------------------------------------------------- */

static void pid_path(char *buf, size_t len)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");

	if (rt && *rt)
		snprintf(buf, len, "%s/my-reader.pid", rt);
	else
		snprintf(buf, len, "/tmp/my-reader.%d", (int)getuid());
}

/* The running reader's PID, or 0 */
static pid_t running(void)
{
	char path[256], exe[64], link[256];
	FILE *f;
	int pid = 0;
	ssize_t n;

	pid_path(path, sizeof path);
	if (!(f = fopen(path, "r")))
		return 0;
	if (fscanf(f, "%d", &pid) != 1)
		pid = 0;
	fclose(f);
	snprintf(exe, sizeof exe, "/proc/%d/exe", pid);
	n = readlink(exe, link, sizeof link - 1);
	if (pid <= 0 || n <= 0)
		return 0;
	link[n] = '\0';
	return strstr(link, "my-reader") ? pid : 0;
}

static void remember(int on)
{
	struct settings s;

	settings_load(&s, NULL);
	if (s.screen_reader != on) {
		s.screen_reader = on;
		settings_save(&s);
	}
}

static void quit(int sig)
{
	char path[256];

	(void)sig;
	hush();
	pid_path(path, sizeof path);
	unlink(path);
	_exit(0);
}

static void pick_voice(void)
{
	const char *v = getenv("MY_SPEAK_VOICE"), *lang = getenv("LANGUAGE");

	if (v && *v && strspn(v, "abcdefghijklmnopqrstuvwxyz-+0123456789") == strlen(v))
		snprintf(voice, sizeof voice, "%s", v);
	else if (lang && strlen(lang) >= 2 && g_ascii_isalpha(lang[0]) && g_ascii_isalpha(lang[1]))
		snprintf(voice, sizeof voice, "%c%c", g_ascii_tolower(lang[0]), g_ascii_tolower(lang[1]));
}

static int run(void)
{
	char path[256];
	FILE *f;
	GMainLoop *loop;
	GIOChannel *ch;

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "my-reader: no X display\n");
		return 1;
	}
	root = DefaultRootWindow(dpy);
	a_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	a_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_reader = XInternAtom(dpy, "_MY_READER_TEXT", False);
	XSelectInput(dpy, root, PropertyChangeMask);

	pid_path(path, sizeof path);
	if ((f = fopen(path, "w"))) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
	signal(SIGTERM, quit);
	signal(SIGINT, quit);
	signal(SIGCHLD, reap);
	pick_voice();

	if (atspi_init() < 0)
		fprintf(stderr, "my-reader: no accessibility bus: only window titles\n");
	atspi_event_listener_register(atspi_event_listener_new(on_focus, NULL, NULL),
	                              "object:state-changed:focused", NULL);
	atspi_event_listener_register(atspi_event_listener_new(on_selected, NULL, NULL),
	                              "object:state-changed:selected", NULL);
	atspi_event_listener_register(atspi_event_listener_new(on_insert, NULL, NULL),
	                              "object:text-changed:insert", NULL);

	ch = g_io_channel_unix_new(ConnectionNumber(dpy));
	g_io_add_watch(ch, G_IO_IN, on_x, NULL);
	say(_("Screen reader on"));
	active_window_changed();
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	pid_t pid = running();

	i18n_init();
	if (!strcmp(mode, "--stop") || (!strcmp(mode, "--toggle") && pid)) {
		if (pid)
			kill(pid, SIGTERM);
		remember(0);
		if (!strcmp(mode, "--toggle")) {
			pick_voice();
			say(_("Screen reader off"));
			if (speaker > 0)
				waitpid(speaker, NULL, 0);
		}
		return 0;
	}
	if (pid)
		return 0;                         /* runs already */
	if (!strcmp(mode, "--autostart")) {
		struct settings s;

		settings_load(&s, NULL);
		if (!s.screen_reader)
			return 0;
	} else if (!strcmp(mode, "--toggle")) {
		remember(1);
	} else if (mode[0]) {
		fprintf(stderr, "usage: my-reader [--toggle | --autostart | --stop]\n");
		return 2;
	}
	/* on its own, in the background */
	if (strcmp(mode, "") != 0 && fork() > 0)
		return 0;
	setsid();
	return run();
}
