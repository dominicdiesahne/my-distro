/*
 * my-setup: the first-boot setup and the "Users and system" window of
 * my-distro (GTK 3). Everything it changes belongs to the whole system, so it
 * runs as root.
 *
 *   my-setup --first-boot   my-login runs this before the first login screen
 *                           (no window manager yet, so it covers the screen):
 *                           keyboard, your account, computer name and theme.
 *                           Then it writes /etc/my-distro/setup-done.
 *   my-setup --manage       users (add, remove, passwords), computer name,
 *                           keyboard, the administrator password and System
 *                           update (runs my-update). Started by my-admin
 *                           after it checked the root password.
 *   my-setup [--update]     (as a normal user) asks for the root password and
 *                           starts my-admin, which starts my-setup --manage
 *                           (--update: on the System update page; the
 *                           taskbar's "Update" button does that).
 *
 * How the system is changed:
 *   - users: busybox adduser / deluser, the home folder is a copy of
 *     /etc/skel; passwords are hashed here with crypt() (SHA-256, "$5$")
 *     and written into /etc/shadow
 *   - computer name: /etc/hostname, /etc/hosts and the `hostname` command
 *   - keyboard: /etc/X11/xorg.conf.d/00-keyboard.conf for the next start of
 *     X, and `setxkbmap` for the X server that runs now
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <ctype.h>
#include <grp.h>
#include <gtk/gtk.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "settings.h"

#define SETUP_DONE    "/etc/my-distro/setup-done"
#define KEYBOARD_CONF "/etc/X11/xorg.conf.d/00-keyboard.conf"
#define DEMO_USER     "dominic"        /* the live system's account (users.table) */
#define FIRST_UID     1000             /* normal users have this uid or higher */

static const struct { const char *id, *label; } layouts[] = {
	{ "de", "German" },
	{ "at", "German (Austria)" },
	{ "ch", "German (Switzerland)" },
	{ "us", "English (US)" },
	{ "gb", "English (UK)" },
	{ "fr", "French" },
	{ "it", "Italian" },
	{ "es", "Spanish" },
	{ "nl", "Dutch" },
	{ "pl", "Polish" },
	{ "cz", "Czech" },
	{ "hu", "Hungarian" },
	{ "ro", "Romanian" },
	{ "tr", "Turkish" },
	{ NULL, NULL }
};

/* --- Checking what was typed ------------------------------------------------ */

/* User names: a-z, 0-9, _ and -, starting with a letter or _, at most 32. */
static int valid_username(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n > 32 || !(islower((unsigned char)s[0]) || s[0] == '_'))
		return 0;
	for (size_t i = 0; i < n; i++)
		if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) ||
		      s[i] == '_' || s[i] == '-'))
			return 0;
	return 1;
}

/* The full name goes into /etc/passwd, where ':' separates the fields. */
static int valid_fullname(const char *s)
{
	if (strlen(s) > 64)
		return 0;
	for (; *s; s++)
		if (*s == ':' || (unsigned char)*s < 0x20 || *s == 0x7f)
			return 0;
	return 1;
}

/* Computer names: letters, digits and -, not starting with -, at most 63. */
static int valid_hostname(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n > 63 || s[0] == '-')
		return 0;
	for (size_t i = 0; i < n; i++)
		if (!isalnum((unsigned char)s[i]) && s[i] != '-')
			return 0;
	return 1;
}

/* --- Changing the system ------------------------------------------------------- */

/* Runs a program (full path, no shell) and waits. 0 = it worked. */
static int run(const char *const argv[])
{
	int status;

	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL,
	                  NULL, NULL, NULL, NULL, &status, NULL))
		return -1;
	return g_spawn_check_wait_status(status, NULL) ? 0 : -1;
}

/* Writes a whole file: a new file first, then renamed over the old one, with
 * the old file's owner and permissions (/etc/shadow must stay private). */
static int write_file(const char *path, const char *text, mode_t mode)
{
	char tmp[512];
	struct stat st;
	FILE *f;

	snprintf(tmp, sizeof tmp, "%s.new", path);
	f = fopen(tmp, "w");
	if (!f)
		return -1;
	if (stat(path, &st) == 0) {
		fchmod(fileno(f), st.st_mode & 07777);
		if (fchown(fileno(f), st.st_uid, st.st_gid) != 0) {
			/* we are root: doesn't fail in practice */
		}
	} else {
		fchmod(fileno(f), mode);
	}
	fputs(text, f);
	if (fclose(f) != 0 || rename(tmp, path) != 0) {
		remove(tmp);
		return -1;
	}
	return 0;
}

/*
 * /etc/passwd and /etc/shadow have one line per user, fields split by ':'.
 * Sets field number `field` (0 = the name) of the user's line.
 * 0 = done, -1 = no such user or the file could not be written.
 */
static int set_field(const char *path, const char *user, int field, const char *value)
{
	char *text, *out;
	char **lines;
	int found = 0;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return -1;
	lines = g_strsplit(text, "\n", -1);
	g_free(text);
	for (int i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], ":", -1);

		if (f[0] && strcmp(f[0], user) == 0 && (int)g_strv_length(f) > field) {
			g_free(f[field]);
			f[field] = g_strdup(value);
			g_free(lines[i]);
			lines[i] = g_strjoinv(":", f);
			found = 1;
		}
		g_strfreev(f);
	}
	out = g_strjoinv("\n", lines);
	g_strfreev(lines);
	if (found && write_file(path, out, 0600) != 0)
		found = 0;
	g_free(out);
	return found ? 0 : -1;
}

/* Hashes the password with a new random salt and puts it into /etc/shadow. */
static int set_password(const char *user, const char *password)
{
	const char *salt = crypt_gensalt("$5$", 0, NULL, 0);   /* SHA-256 */
	char *hash = salt ? crypt(password, salt) : NULL;
	char line[512];
	FILE *f;

	if (!hash || hash[0] == '*')
		return -1;
	if (set_field("/etc/shadow", user, 1, hash) == 0)
		return 0;
	/* no line for this user in /etc/shadow yet: add one */
	snprintf(line, sizeof line, "%s:%s:%ld:0:99999:7:::\n", user, hash,
	         (long)(time(NULL) / 86400));
	f = fopen("/etc/shadow", "a");
	if (!f)
		return -1;
	fputs(line, f);
	return fclose(f) == 0 ? 0 : -1;
}

/* chown -R user:group on the home folder */
static void give_home(const char *home, uid_t uid, gid_t gid)
{
	char owner[32];
	const char *argv[] = { "/bin/chown", "-R", owner, home, NULL };

	snprintf(owner, sizeof owner, "%u:%u", (unsigned)uid, (unsigned)gid);
	run(argv);
}

/* The new user's theme: one line in their own settings file. */
static void set_theme(const char *home, const char *theme)
{
	char path[512], dir[512], text[64];

	snprintf(dir, sizeof dir, "%s/.config/my-distro", home);
	g_mkdir_with_parents(dir, 0755);
	settings_user_path(path, sizeof path, home);
	snprintf(text, sizeof text, "theme = %s\n", theme);
	write_file(path, text, 0644);
}

/*
 * Makes a new user, or updates an existing normal user (full name, password).
 * theme may be NULL. Returns NULL when it worked, else what went wrong.
 */
static const char *save_user(const char *name, const char *fullname,
                             const char *password, const char *theme, int must_be_new)
{
	struct passwd *pw = getpwnam(name);
	char home[300];

	if (pw && pw->pw_uid < FIRST_UID)
		return "That user name belongs to the system. Choose another one.";
	if (pw && must_be_new)
		return "That user already exists.";
	if (!pw) {
		snprintf(home, sizeof home, "/home/%s", name);
		const char *add[] = { "/usr/sbin/adduser", "-D", "-H", "-h", home,
		                      "-g", fullname, "-s", "/bin/sh", name, NULL };
		const char *skel[] = { "/bin/cp", "-a", "/etc/skel", home, NULL };

		if (run(add) != 0 || !(pw = getpwnam(name)))
			return "Could not create the user.";
		if (access(home, F_OK) != 0 && run(skel) != 0)
			g_mkdir_with_parents(home, 0755);
	} else {
		snprintf(home, sizeof home, "%s", pw->pw_dir);
		set_field("/etc/passwd", name, 4, fullname);
	}
	uid_t uid = pw->pw_uid;
	gid_t gid = pw->pw_gid;

	if (set_password(name, password) != 0)
		return "Could not set the password.";
	/* netdev: may choose Wi-Fi networks (wpa_supplicant's control socket);
	 * audio: may use the sound card (/dev/snd) */
	const char *netdev[] = { "/usr/sbin/addgroup", name, "netdev", NULL };
	const char *audio[] = { "/usr/sbin/addgroup", name, "audio", NULL };

	run(netdev);
	run(audio);
	if (theme)
		set_theme(home, theme);
	give_home(home, uid, gid);
	return NULL;
}

/* Administrators are the members of the group wheel: they may use sudo
 * (/etc/sudoers.d/wheel). */
static int is_admin(const char *name)
{
	struct group *g = getgrnam("wheel");

	for (char **m = g ? g->gr_mem : NULL; m && *m; m++)
		if (strcmp(*m, name) == 0)
			return 1;
	return 0;
}

static const char *set_admin(const char *name, int admin)
{
	const char *add[] = { "/usr/sbin/addgroup", name, "wheel", NULL };
	const char *del[] = { "/usr/sbin/delgroup", name, "wheel", NULL };

	if (run(admin ? add : del) != 0)
		return "Could not change the administrator rights.";
	return NULL;
}

/* Removes a normal user and their home folder. */
static const char *remove_user(const char *name)
{
	struct passwd *pw = getpwnam(name);
	char home[300];
	const char *del[] = { "/usr/sbin/deluser", name, NULL };

	if (!pw || pw->pw_uid < FIRST_UID)
		return "No such user.";
	snprintf(home, sizeof home, "%s", pw->pw_dir);
	if (run(del) != 0)
		return "Could not remove the user.";
	/* only a folder in /home that is clearly theirs */
	if (strncmp(home, "/home/", 6) == 0 && !strstr(home, "..") && strchr(home + 6, '/') == NULL) {
		const char *rm[] = { "/bin/rm", "-rf", home, NULL };

		run(rm);
	}
	return NULL;
}

static const char *set_hostname(const char *name)
{
	char line[128], *text, *out;
	char **lines;
	int found = 0;
	const char *cmd[] = { "/bin/hostname", name, NULL };

	snprintf(line, sizeof line, "%s\n", name);
	if (write_file("/etc/hostname", line, 0644) != 0)
		return "Could not write /etc/hostname.";
	run(cmd);

	/* /etc/hosts: the line "127.0.1.1 <name>" lets programs find this computer */
	if (g_file_get_contents("/etc/hosts", &text, NULL, NULL)) {
		lines = g_strsplit(text, "\n", -1);
		g_free(text);
		for (int i = 0; lines[i]; i++)
			if (g_str_has_prefix(lines[i], "127.0.1.1")) {
				g_free(lines[i]);
				lines[i] = g_strdup_printf("127.0.1.1\t%s", name);
				found = 1;
			}
		out = g_strjoinv("\n", lines);
		g_strfreev(lines);
		if (!found) {
			char *more = g_strdup_printf("%s%s127.0.1.1\t%s\n", out,
			                             g_str_has_suffix(out, "\n") ? "" : "\n", name);
			g_free(out);
			out = more;
		}
		write_file("/etc/hosts", out, 0644);
		g_free(out);
	}
	return NULL;
}

static void current_layout(char *buf, size_t len)
{
	char *text, *p;

	snprintf(buf, len, "de");
	if (!g_file_get_contents(KEYBOARD_CONF, &text, NULL, NULL))
		return;
	p = strstr(text, "\"XkbLayout\"");
	if (p && (p = strchr(p + 11, '"'))) {
		char *end = strchr(p + 1, '"');

		if (end && end - p - 1 < (long)len) {
			*end = '\0';
			snprintf(buf, len, "%s", p + 1);
		}
	}
	g_free(text);
}

static const char *set_layout(const char *id)
{
	char *text = g_strdup_printf(
		"# Keyboard layout, set in the setup or in \"Users and system\".\n"
		"Section \"InputClass\"\n"
		"    Identifier \"keyboard\"\n"
		"    MatchIsKeyboard \"on\"\n"
		"    Option \"XkbLayout\" \"%s\"\n"
		"EndSection\n", id);
	const char *now[] = { "/usr/bin/setxkbmap", "-layout", id, NULL };
	int ok = write_file(KEYBOARD_CONF, text, 0644) == 0;

	g_free(text);
	run(now);                         /* the X server that runs now */
	return ok ? NULL : "Could not save the keyboard layout.";
}

static void write_setup_done(const char *how)
{
	char text[64];

	snprintf(text, sizeof text, "%s\n", how);
	write_file(SETUP_DONE, text, 0644);
}

/* Started from the live ISO (root filesystem in RAM)? Same test as my-install. */
static int live_system(void)
{
	char *cmdline = NULL;
	int live = 0;

	if (g_file_get_contents("/proc/cmdline", &cmdline, NULL, NULL))
		live = strstr(cmdline, "rdinit=") != NULL;
	g_free(cmdline);
	return live;
}

/* --- Small helpers for building windows -------------------------------------- */

static GtkWidget *label(const char *text, const char *css_class)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 46);
	if (css_class)
		gtk_style_context_add_class(gtk_widget_get_style_context(l), css_class);
	return l;
}

static GtkWidget *entry(int secret)
{
	GtkWidget *e = gtk_entry_new();

	gtk_entry_set_visibility(GTK_ENTRY(e), !secret);
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	return e;
}

static GtkWidget *layout_combo(void)
{
	GtkWidget *c = gtk_combo_box_text_new();
	char now[16];

	for (int i = 0; layouts[i].id; i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), layouts[i].id, layouts[i].label);
	current_layout(now, sizeof now);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), now)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), now, now);  /* set by hand */
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), now);
	}
	return c;
}

static void on_layout(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (id)
		set_layout(id);
}

static const char *text_of(GtkWidget *e)
{
	return gtk_entry_get_text(GTK_ENTRY(e));
}

/* Checks two password fields. NULL = fine. */
static const char *check_passwords(GtkWidget *a, GtkWidget *b)
{
	if (!text_of(a)[0])
		return "Choose a password.";
	if (strcmp(text_of(a), text_of(b)) != 0)
		return "The two passwords are not the same.";
	return NULL;
}

static void show_error(GtkWidget *l, const char *msg)
{
	gtk_label_set_text(GTK_LABEL(l), msg ? msg : "");
}

/* --- First-boot setup ----------------------------------------------------------- */

static GtkWidget *fb_stack, *fb_fullname, *fb_username, *fb_pass1, *fb_pass2,
	*fb_same_root, *fb_root_box, *fb_root1, *fb_root2, *fb_hostname,
	*fb_err_account, *fb_err_computer, *fb_done_text;
static GtkCssProvider *fb_css;
static const char *fb_theme = "teal";
static int fb_username_edited, fb_filling_username;

/* The window's colors follow the theme chosen on the "computer" page. */
static void fb_style(void)
{
	struct settings s;
	char wall[512], *css;

	memset(&s, 0, sizeof s);
	settings_load(&s, "/nonexistent");          /* system defaults only */
	for (int i = 0; themes[i].name; i++)
		if (strcmp(themes[i].name, fb_theme) == 0)
			s.pal = themes[i];
	g_strlcpy(s.theme, fb_theme, sizeof s.theme);
	strcpy(s.wallpaper, "theme");
	settings_wallpaper_file(&s, wall, sizeof wall);
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
	             s.pal.dark, NULL);
	css = g_strdup_printf(
		"window.setup { background-color: %s; background-image: url('%s');"
		"  background-size: cover; background-position: center; }"
		".card { background-color: alpha(%s, 0.93); border-top: 2px solid %s;"
		"  padding: 30px 34px; }"
		".card label { color: %s; }"
		".card label.dim { color: %s; }"
		".card label.title { font-size: 20pt; font-weight: bold; }"
		".card label.error { color: %s; }"
		".card button.suggested-action { background-image: none;"
		"  background-color: %s; color: %s; border-color: %s; }"
		".card button.suggested-action label { color: %s; }"
		".card button.theme:checked { background-image: none;"
		"  background-color: %s; border-color: %s; }",
		s.pal.bg, wall, s.pal.bg, s.pal.accent, s.pal.text, s.pal.dim,
		s.pal.dark ? "#ff9a8f" : "#b3261e", s.pal.accent, s.pal.on_accent,
		s.pal.accent, s.pal.on_accent, s.pal.active, s.pal.accent);
	gtk_css_provider_load_from_data(fb_css, css, -1, NULL);
	g_free(css);
}

static void fb_go(const char *page)
{
	gtk_stack_set_visible_child_name(GTK_STACK(fb_stack), page);
}

/* "Dominic Matei" -> "dominic" until the user name is typed by hand */
static void on_fullname(GtkEditable *e, gpointer d)
{
	char name[33] = "";
	const char *s = text_of(GTK_WIDGET(e));
	size_t n = 0;

	(void)d;
	if (fb_username_edited)
		return;
	for (; *s && *s != ' ' && n < sizeof name - 1; s++)
		if (isalpha((unsigned char)*s) || (n > 0 && isdigit((unsigned char)*s)))
			name[n++] = tolower((unsigned char)*s);
	name[n] = '\0';
	fb_filling_username = 1;
	gtk_entry_set_text(GTK_ENTRY(fb_username), name);
	fb_filling_username = 0;
}

static void on_username(GtkEditable *e, gpointer d)
{
	(void)e; (void)d;
	if (!fb_filling_username)
		fb_username_edited = 1;
}

static void on_same_root(GtkToggleButton *b, gpointer d)
{
	(void)d;
	gtk_widget_set_visible(fb_root_box, !gtk_toggle_button_get_active(b));
}

static void on_theme_button(GtkToggleButton *b, gpointer name)
{
	if (gtk_toggle_button_get_active(b)) {
		fb_theme = name;
		fb_style();
	}
}

static void on_account_next(GtkButton *b, gpointer d)
{
	const char *user = text_of(fb_username), *err = NULL;
	struct passwd *pw;

	(void)b; (void)d;
	if (!valid_username(user))
		err = "User name: small letters a-z, digits, - and _, starting with a letter.";
	else if ((pw = getpwnam(user)) && pw->pw_uid < FIRST_UID)
		err = "That user name belongs to the system. Choose another one.";
	else if (!valid_fullname(text_of(fb_fullname)))
		err = "The name can't contain \":\".";
	else
		err = check_passwords(fb_pass1, fb_pass2);
	if (!err && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb_same_root)) &&
	    check_passwords(fb_root1, fb_root2))
		err = "Administrator password: type the same password twice.";
	show_error(fb_err_account, err);
	if (!err)
		fb_go("computer");
}

static void on_finish(GtkButton *b, gpointer d)
{
	const char *user = text_of(fb_username), *err;
	const char *root_pw = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb_same_root))
	                      ? text_of(fb_pass1) : text_of(fb_root1);
	char *fullname, *msg;

	(void)b; (void)d;
	if (!valid_hostname(text_of(fb_hostname))) {
		show_error(fb_err_computer, "Computer name: letters, digits and -, no spaces.");
		return;
	}
	fullname = g_strdup(text_of(fb_fullname)[0] ? text_of(fb_fullname) : user);
	err = save_user(user, fullname, text_of(fb_pass1), fb_theme, 0);
	g_free(fullname);
	if (!err && set_password("root", root_pw) != 0)
		err = "Could not set the administrator password.";
	/* Your password is the root password too: you are the administrator,
	 * so you may also use sudo. With a separate root password you aren't
	 * (that's what a separate one is for). */
	if (!err) {
		int same = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb_same_root));

		if (same != is_admin(user))
			set_admin(user, same);
	}
	if (err) {
		show_error(fb_err_computer, err);
		return;
	}
	set_hostname(text_of(fb_hostname));
	/* The demo account has a password everyone knows: remove it. */
	if (strcmp(user, DEMO_USER) != 0)
		remove_user(DEMO_USER);
	write_setup_done("done");

	msg = g_strdup_printf("Your account \"%s\" is ready. Log in with your new password.",
	                      user);
	gtk_label_set_text(GTK_LABEL(fb_done_text), msg);
	g_free(msg);
	fb_go("done");
}

static void on_skip(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	write_setup_done("skipped");
	gtk_main_quit();
}

static void on_page(GtkButton *b, gpointer page)
{
	(void)b;
	fb_go(page);
}

/* A row of buttons at the bottom of a page: back (optional) left, next right */
static GtkWidget *buttons(const char *back_label, GCallback back, gpointer back_data,
                          const char *next_label, GCallback next, gpointer next_data)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *n = gtk_button_new_with_label(next_label);

	gtk_widget_set_margin_top(box, 10);
	if (back_label) {
		GtkWidget *bk = gtk_button_new_with_label(back_label);

		g_signal_connect(bk, "clicked", back, back_data);
		gtk_box_pack_start(GTK_BOX(box), bk, FALSE, FALSE, 0);
	}
	gtk_style_context_add_class(gtk_widget_get_style_context(n), "suggested-action");
	gtk_widget_set_can_default(n, TRUE);
	g_signal_connect(n, "clicked", next, next_data);
	gtk_box_pack_end(GTK_BOX(box), n, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(box), "next", n);
	return box;
}

static GtkWidget *fb_page(const char *name, const char *title, const char *intro)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

	gtk_box_pack_start(GTK_BOX(box), label(title, "title"), FALSE, FALSE, 0);
	if (intro)
		gtk_box_pack_start(GTK_BOX(box), label(intro, "dim"), FALSE, FALSE, 6);
	gtk_stack_add_named(GTK_STACK(fb_stack), box, name);
	return box;
}

static void add_field(GtkWidget *box, const char *caption, GtkWidget *w)
{
	GtkWidget *l = label(caption, "dim");

	gtk_widget_set_margin_top(l, 6);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
}

/* The page's "next" button reacts to Enter in its text fields. */
static void on_page_shown(GObject *stack, GParamSpec *ps, gpointer d)
{
	GtkWidget *page = gtk_stack_get_visible_child(GTK_STACK(stack));
	GtkWidget *next = g_object_get_data(G_OBJECT(page), "next");

	(void)ps; (void)d;
	if (next)
		gtk_widget_grab_default(next);
}

static void set_next(GtkWidget *page, GtkWidget *button_row)
{
	gtk_box_pack_end(GTK_BOX(page), button_row, FALSE, FALSE, 0);
	g_object_set_data(G_OBJECT(page), "next", g_object_get_data(G_OBJECT(button_row), "next"));
}

static int first_boot(void)
{
	GtkWidget *win, *center, *card, *p, *row, *flow;
	GdkRectangle screen = { 0, 0, 1024, 768 };
	GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
	GSList *group = NULL;
	char host[64] = "my-distro";
	int live = live_system();

	if (!mon)
		mon = gdk_display_get_monitor(gdk_display_get_default(), 0);
	if (mon)
		gdk_monitor_get_geometry(mon, &screen);

	fb_css = gtk_css_provider_new();
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(fb_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	fb_style();

	/* No window manager runs yet: the window covers the screen by itself. */
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Setup");
	gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
	gtk_window_set_default_size(GTK_WINDOW(win), screen.width, screen.height);
	gtk_window_move(GTK_WINDOW(win), screen.x, screen.y);
	gtk_style_context_add_class(gtk_widget_get_style_context(win), "setup");

	center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
	gtk_widget_set_size_request(card, 480, -1);
	gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
	gtk_widget_set_vexpand(card, TRUE);
	gtk_box_pack_start(GTK_BOX(center), card, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(win), center);

	fb_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(fb_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	gtk_stack_set_vhomogeneous(GTK_STACK(fb_stack), FALSE);
	gtk_stack_set_interpolate_size(GTK_STACK(fb_stack), TRUE);
	g_signal_connect(fb_stack, "notify::visible-child", G_CALLBACK(on_page_shown), NULL);
	gtk_box_pack_start(GTK_BOX(card), fb_stack, FALSE, FALSE, 0);

	/* 1. Welcome and keyboard (first, so the passwords are typed right) */
	p = fb_page("welcome", "Welcome to my-distro",
	            "Let's set up your computer: your keyboard, your own account and a "
	            "name for the computer. It takes a minute, and you can change all of "
	            "it later in \"Users and system\".");
	row = layout_combo();
	g_signal_connect(row, "changed", G_CALLBACK(on_layout), NULL);
	add_field(p, "Keyboard layout", row);
	add_field(p, "Try your keyboard here", entry(0));
	if (live) {
		GtkWidget *skip = gtk_button_new_with_label("Skip: try it with the demo account");

		gtk_widget_set_halign(skip, GTK_ALIGN_START);
		gtk_widget_set_margin_top(skip, 14);
		gtk_button_set_relief(GTK_BUTTON(skip), GTK_RELIEF_NONE);
		g_signal_connect(skip, "clicked", G_CALLBACK(on_skip), NULL);
		gtk_box_pack_start(GTK_BOX(p), skip, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(p),
			label("The live system keeps nothing after a reboot. Install it to "
			      "keep your account.", "dim"), FALSE, FALSE, 0);
	}
	set_next(p, buttons(NULL, NULL, NULL, "Next", G_CALLBACK(on_page), "account"));

	/* 2. Account */
	p = fb_page("account", "Your account", NULL);
	fb_fullname = entry(0);
	fb_username = entry(0);
	fb_pass1 = entry(1);
	fb_pass2 = entry(1);
	g_signal_connect(fb_fullname, "changed", G_CALLBACK(on_fullname), NULL);
	g_signal_connect(fb_username, "changed", G_CALLBACK(on_username), NULL);
	add_field(p, "Your name", fb_fullname);
	add_field(p, "User name (to log in)", fb_username);
	add_field(p, "Password", fb_pass1);
	add_field(p, "Password again", fb_pass2);
	fb_same_root = gtk_check_button_new_with_label(
		"Use this password for the administrator (root) too");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fb_same_root), TRUE);
	gtk_widget_set_margin_top(fb_same_root, 10);
	g_signal_connect(fb_same_root, "toggled", G_CALLBACK(on_same_root), NULL);
	gtk_box_pack_start(GTK_BOX(p), fb_same_root, FALSE, FALSE, 0);
	fb_root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	fb_root1 = entry(1);
	fb_root2 = entry(1);
	add_field(fb_root_box, "Administrator password", fb_root1);
	add_field(fb_root_box, "Administrator password again", fb_root2);
	gtk_box_pack_start(GTK_BOX(p), fb_root_box, FALSE, FALSE, 0);
	/* hidden until "same password" is unticked; show_all below skips it */
	gtk_widget_show_all(fb_root_box);
	gtk_widget_hide(fb_root_box);
	gtk_widget_set_no_show_all(fb_root_box, TRUE);
	fb_err_account = label("", "error");
	gtk_box_pack_start(GTK_BOX(p), fb_err_account, FALSE, FALSE, 4);
	set_next(p, buttons("Back", G_CALLBACK(on_page), "welcome",
	                    "Next", G_CALLBACK(on_account_next), NULL));

	/* 3. Computer name and theme */
	p = fb_page("computer", "Your computer", NULL);
	fb_hostname = entry(0);
	gethostname(host, sizeof host - 1);
	gtk_entry_set_text(GTK_ENTRY(fb_hostname), host);
	add_field(p, "Computer name (shown on the network and the login screen)", fb_hostname);
	flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 3);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 3);
	for (int i = 0; themes[i].name; i++) {
		GtkWidget *b = gtk_radio_button_new_with_label(group, themes[i].label);

		group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(b));
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
		gtk_style_context_add_class(gtk_widget_get_style_context(b), "theme");
		g_signal_connect(b, "toggled", G_CALLBACK(on_theme_button), (gpointer)themes[i].name);
		gtk_container_add(GTK_CONTAINER(flow), b);
	}
	add_field(p, "Colors (more choices later in Settings)", flow);
	fb_err_computer = label("", "error");
	gtk_box_pack_start(GTK_BOX(p), fb_err_computer, FALSE, FALSE, 4);
	set_next(p, buttons("Back", G_CALLBACK(on_page), "account",
	                    "Finish", G_CALLBACK(on_finish), NULL));

	/* 4. Done */
	p = fb_page("done", "All set", NULL);
	fb_done_text = label("", NULL);
	gtk_box_pack_start(GTK_BOX(p), fb_done_text, FALSE, FALSE, 0);
	set_next(p, buttons(NULL, NULL, NULL, "Go to the login screen",
	                    G_CALLBACK(gtk_main_quit), NULL));

	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(win);
	gtk_window_present(GTK_WINDOW(win));   /* no window manager: takes the focus */
	on_page_shown(G_OBJECT(fb_stack), NULL, NULL);
	gtk_main();
	return 0;
}

/* --- "Users and system" ------------------------------------------------------------ */

static GtkWidget *mg_admin;
static GtkWidget *mg_win, *mg_users, *mg_remove, *mg_status, *mg_hostname,
	*mg_root1, *mg_root2;
static const char *mg_me;          /* who started it (can't remove themselves) */

static void status(const char *err, const char *ok)
{
	gtk_label_set_text(GTK_LABEL(mg_status), err ? err : ok);
}

static const char *selected_user(void)
{
	GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(mg_users));

	return r ? g_object_get_data(G_OBJECT(r), "name") : NULL;
}

static void on_user_selected(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	const char *name = r ? g_object_get_data(G_OBJECT(r), "name") : NULL;

	(void)l; (void)d;
	gtk_widget_set_sensitive(mg_remove, name && strcmp(name, mg_me) != 0);
	gtk_widget_set_sensitive(mg_admin, name != NULL);
	gtk_button_set_label(GTK_BUTTON(mg_admin), name && is_admin(name)
	                     ? "Remove administrator rights" : "Make administrator");
}

static void fill_users(void)
{
	struct passwd *pw;
	GList *old = gtk_container_get_children(GTK_CONTAINER(mg_users));

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);

	setpwent();
	while ((pw = getpwent())) {
		GtkWidget *row, *l;
		char *text;

		if (pw->pw_uid < FIRST_UID || pw->pw_uid >= 60000)
			continue;
		text = g_markup_printf_escaped("<b>%s</b>   %s%s%s", pw->pw_name,
			pw->pw_gecos ? pw->pw_gecos : "",
			strcmp(pw->pw_name, mg_me) == 0 ? "   (you)" : "",
			is_admin(pw->pw_name) ? "   · administrator" : "");
		l = gtk_label_new(NULL);
		gtk_label_set_markup(GTK_LABEL(l), text);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		g_object_set(l, "margin", 8, NULL);
		g_free(text);
		row = gtk_list_box_row_new();
		gtk_container_add(GTK_CONTAINER(row), l);
		g_object_set_data_full(G_OBJECT(row), "name", g_strdup(pw->pw_name), g_free);
		gtk_list_box_insert(GTK_LIST_BOX(mg_users), row, -1);
	}
	endpwent();
	gtk_widget_show_all(mg_users);
	gtk_widget_set_sensitive(mg_remove, FALSE);
	gtk_widget_set_sensitive(mg_admin, FALSE);
	gtk_button_set_label(GTK_BUTTON(mg_admin), "Make administrator");
}

/* Administrator rights (sudo) on or off for the chosen user */
static void on_admin(GtkButton *b, gpointer d)
{
	const char *user = selected_user();
	char *name;
	int admin;

	(void)b; (void)d;
	if (!user)
		return;
	name = g_strdup(user);              /* fill_users() frees the row's copy */
	admin = !is_admin(name);
	status(set_admin(name, admin), admin
	       ? "Administrator now: may use sudo with their own password."
	       : "No administrator rights any more.");
	fill_users();
	g_free(name);
}

/* A dialog with text fields; returns TRUE when OK was pressed. */
static GtkWidget *form_dialog(const char *title, GtkWidget **grid)
{
	GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(mg_win),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		"Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);

	*grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(*grid), 10);
	gtk_grid_set_column_spacing(GTK_GRID(*grid), 12);
	gtk_container_set_border_width(GTK_CONTAINER(*grid), 18);
	gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(d))), *grid);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	return d;
}

static void grid_row(GtkWidget *grid, int y, const char *caption, GtkWidget *w)
{
	GtkWidget *l = gtk_label_new(caption);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_grid_attach(GTK_GRID(grid), l, 0, y, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), w, 1, y, 1, 1);
}

static void on_add_user(GtkButton *b, gpointer d)
{
	GtkWidget *grid, *dlg = form_dialog("Add a user", &grid);
	GtkWidget *full = entry(0), *name = entry(0), *p1 = entry(1), *p2 = entry(1);
	GtkWidget *err = label("", "error");

	(void)b; (void)d;
	grid_row(grid, 0, "Full name", full);
	grid_row(grid, 1, "User name", name);
	grid_row(grid, 2, "Password", p1);
	grid_row(grid, 3, "Password again", p2);
	gtk_grid_attach(GTK_GRID(grid), err, 0, 4, 2, 1);
	gtk_widget_show_all(dlg);
	while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
		const char *e = NULL;

		if (!valid_username(text_of(name)))
			e = "User name: small letters a-z, digits, - and _, starting with a letter.";
		else if (!valid_fullname(text_of(full)))
			e = "The full name can't contain \":\".";
		else if (!(e = check_passwords(p1, p2)))
			e = save_user(text_of(name), text_of(full)[0] ? text_of(full) : text_of(name),
			              text_of(p1), NULL, 1);
		if (!e) {
			status(NULL, "User added.");
			fill_users();
			break;
		}
		gtk_label_set_text(GTK_LABEL(err), e);
	}
	gtk_widget_destroy(dlg);
}

static void on_password(GtkButton *b, gpointer d)
{
	const char *user = selected_user();
	GtkWidget *grid, *dlg, *p1 = entry(1), *p2 = entry(1), *err = label("", "error");
	char *title;

	(void)b; (void)d;
	if (!user) {
		status("Choose a user in the list first.", NULL);
		return;
	}
	title = g_strdup_printf("New password for %s", user);
	dlg = form_dialog(title, &grid);
	g_free(title);
	grid_row(grid, 0, "New password", p1);
	grid_row(grid, 1, "Password again", p2);
	gtk_grid_attach(GTK_GRID(grid), err, 0, 2, 2, 1);
	gtk_widget_show_all(dlg);
	while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
		const char *e = check_passwords(p1, p2);

		if (!e && set_password(user, text_of(p1)) != 0)
			e = "Could not set the password.";
		if (!e) {
			status(NULL, "Password changed.");
			break;
		}
		gtk_label_set_text(GTK_LABEL(err), e);
	}
	gtk_widget_destroy(dlg);
}

static void on_remove_user(GtkButton *b, gpointer d)
{
	const char *user = selected_user();
	GtkWidget *ask;

	(void)b; (void)d;
	if (!user || strcmp(user, mg_me) == 0)
		return;
	ask = gtk_message_dialog_new(GTK_WINDOW(mg_win), GTK_DIALOG_MODAL,
		GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
		"Remove the user \"%s\"?", user);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask),
		"Their home folder /home/%s and all files in it are deleted.", user);
	gtk_dialog_add_buttons(GTK_DIALOG(ask), "Cancel", GTK_RESPONSE_CANCEL,
	                       "Remove", GTK_RESPONSE_ACCEPT, NULL);
	if (gtk_dialog_run(GTK_DIALOG(ask)) == GTK_RESPONSE_ACCEPT) {
		status(remove_user(user), "User removed.");
		fill_users();
	}
	gtk_widget_destroy(ask);
}

static void on_save_hostname(GtkButton *b, gpointer d)
{
	const char *h = text_of(mg_hostname);

	(void)b; (void)d;
	if (!valid_hostname(h))
		status("Computer name: letters, digits and -, no spaces.", NULL);
	else
		status(set_hostname(h), "Computer name saved.");
}

static void on_layout_manage(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (id)
		status(set_layout(id), "Keyboard layout changed.");
}

static void on_save_root(GtkButton *b, gpointer d)
{
	const char *e = check_passwords(mg_root1, mg_root2);

	(void)b; (void)d;
	if (!e && set_password("root", text_of(mg_root1)) != 0)
		e = "Could not set the password.";
	status(e, "Administrator password changed.");
	if (!e) {
		gtk_entry_set_text(GTK_ENTRY(mg_root1), "");
		gtk_entry_set_text(GTK_ENTRY(mg_root2), "");
	}
}

/* --- System update page: runs my-update and shows what it says ----------------- */

static GtkWidget *up_status, *up_view, *up_check, *up_install, *up_restart, *up_undo;
static GtkTextBuffer *up_buf;
static char up_latest[32];
static char up_previous[32];      /* the version "Undo last update" goes back to */
static int up_undoing;            /* my-update is running "rollback", not "install" */

static void up_append(const char *text)
{
	GtkTextIter end;

	gtk_text_buffer_get_end_iter(up_buf, &end);
	gtk_text_buffer_insert(up_buf, &end, text, -1);
	gtk_text_buffer_get_end_iter(up_buf, &end);
	gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(up_view), &end, 0, FALSE, 0, 0);
}

/* "key=value" from my-update's --porcelain output */
static void porcelain(const char *out, const char *key, char *val, size_t len)
{
	char pat[32];
	const char *p;

	snprintf(pat, sizeof pat, "%s=", key);
	val[0] = '\0';
	for (p = out; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
		if (strncmp(p, pat, strlen(pat)) == 0) {
			size_t n = strcspn(p + strlen(pat), "\n");

			snprintf(val, len, "%.*s", (int)(n < len ? n : len - 1), p + strlen(pat));
			return;
		}
}

static void on_check(GtkButton *b, gpointer d)
{
	const char *argv[] = { "/usr/bin/my-update", "check", "--porcelain", NULL };
	char *out = NULL, *msg, installed[32], available[4], installable[4], live[4], err[200];
	const char *notes;
	int st;

	(void)b; (void)d;
	gtk_label_set_text(GTK_LABEL(up_status), "Looking for updates…");
	gtk_widget_set_sensitive(up_install, FALSE);
	while (gtk_events_pending())
		gtk_main_iteration();
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, &st, NULL) || !out) {
		gtk_label_set_text(GTK_LABEL(up_status), "my-update is missing.");
		return;
	}
	porcelain(out, "installed", installed, sizeof installed);
	porcelain(out, "latest", up_latest, sizeof up_latest);
	porcelain(out, "available", available, sizeof available);
	porcelain(out, "installable", installable, sizeof installable);
	porcelain(out, "live", live, sizeof live);
	porcelain(out, "error", err, sizeof err);
	porcelain(out, "rollback", up_previous, sizeof up_previous);
	if (up_previous[0]) {
		char *t = g_strdup_printf("Undo last update (back to %s)…", up_previous);

		gtk_button_set_label(GTK_BUTTON(up_undo), t);
		g_free(t);
		gtk_widget_show(up_undo);
	} else {
		gtk_widget_hide(up_undo);
	}
	notes = strstr(out, "notes:\n");
	gtk_text_buffer_set_text(up_buf, notes ? notes + 7 : "", -1);

	if (err[0])
		msg = g_strdup_printf("Could not look for updates (no internet?): %s", err);
	else if (strcmp(available, "1") != 0)
		msg = g_strdup_printf("my-distro %s is up to date.", installed);
	else if (strcmp(live, "1") == 0)
		msg = g_strdup_printf("my-distro %s is out. This is the live system, which "
			"forgets updates: download the new ISO from GitHub.", up_latest);
	else if (strcmp(installable, "1") != 0)
		msg = g_strdup_printf("my-distro %s is out, but without an update package: "
			"download its ISO from GitHub.", up_latest);
	else
		msg = g_strdup_printf("my-distro %s is ready to install (you have %s). "
			"What's new:", up_latest, installed);
	gtk_label_set_text(GTK_LABEL(up_status), msg);
	gtk_widget_set_sensitive(up_install, strcmp(installable, "1") == 0);
	g_free(msg);
	g_free(out);
}

static gboolean on_install_output(GIOChannel *ch, GIOCondition cond, gpointer d)
{
	char *line;
	gsize n;

	(void)d;
	if (cond & G_IO_IN)
		while (g_io_channel_read_line(ch, &line, &n, NULL, NULL) == G_IO_STATUS_NORMAL) {
			up_append(line);
			g_free(line);
		}
	return !(cond & (G_IO_HUP | G_IO_ERR));
}

static void on_install_done(GPid pid, gint st, gpointer d)
{
	(void)d;
	g_spawn_close_pid(pid);
	gtk_widget_set_sensitive(up_check, TRUE);
	gtk_widget_set_sensitive(up_undo, TRUE);
	if (g_spawn_check_wait_status(st, NULL)) {
		gtk_label_set_text(GTK_LABEL(up_status), up_undoing
			? "The previous version is back. Restart the computer to finish."
			: "The update is installed. Restart the computer to finish. "
			  "If something doesn't work afterwards, \"Undo last update\" goes back.");
		gtk_widget_show(up_restart);
		if (up_undoing)
			gtk_widget_hide(up_undo);
	} else {
		gtk_label_set_text(GTK_LABEL(up_status), up_undoing
			? "Going back did not work: see the messages below."
			: "The update did not work: see the messages below.");
		gtk_widget_set_sensitive(up_install, !up_undoing);
	}
}

/* Asks first, then runs my-update with that command and shows what it says */
static void run_update(const char *cmd, const char *question, const char *detail,
                       const char *yes, const char *busy)
{
	const char *argv[] = { "/usr/bin/my-update", cmd, NULL };
	GtkWidget *ask;
	GPid pid;
	int out, answer;
	GIOChannel *ch;

	ask = gtk_message_dialog_new(GTK_WINDOW(mg_win), GTK_DIALOG_MODAL,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", question);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask), "%s", detail);
	gtk_dialog_add_buttons(GTK_DIALOG(ask), "Cancel", GTK_RESPONSE_CANCEL,
	                       yes, GTK_RESPONSE_ACCEPT, NULL);
	answer = gtk_dialog_run(GTK_DIALOG(ask));
	gtk_widget_destroy(ask);
	if (answer != GTK_RESPONSE_ACCEPT)
		return;

	up_undoing = strcmp(cmd, "rollback") == 0;
	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL,
	                              G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
	                              NULL, NULL, &pid, NULL, &out, NULL, NULL)) {
		gtk_label_set_text(GTK_LABEL(up_status), "Could not start my-update.");
		return;
	}
	gtk_widget_set_sensitive(up_install, FALSE);
	gtk_widget_set_sensitive(up_check, FALSE);
	gtk_widget_set_sensitive(up_undo, FALSE);
	gtk_label_set_text(GTK_LABEL(up_status), busy);
	gtk_text_buffer_set_text(up_buf, "", -1);
	ch = g_io_channel_unix_new(out);
	g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_install_output, NULL);
	g_io_channel_unref(ch);
	g_child_watch_add(pid, on_install_done, NULL);
}

static void on_install(GtkButton *b, gpointer d)
{
	char *q = g_strdup_printf("Install my-distro %s?", up_latest);

	(void)b; (void)d;
	run_update("install", q, "Your files, users and settings stay. Don't switch the "
	           "computer off while it installs; restart it afterwards.", "Install",
	           "Installing… don't switch the computer off.");
	g_free(q);
}

static void on_undo(GtkButton *b, gpointer d)
{
	char *q = g_strdup_printf("Go back to my-distro %s?", up_previous);

	(void)b; (void)d;
	run_update("rollback", q, "The system files of the version before the last update "
	           "come back. Your files, users and settings stay. Restart afterwards.",
	           "Go back", "Going back… don't switch the computer off.");
	g_free(q);
}

static void on_restart(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	kill(1, SIGTERM);                 /* our init: TERM = reboot */
}

static GtkWidget *mg_page(GtkWidget *stack, const char *name, const char *title)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

	gtk_container_set_border_width(GTK_CONTAINER(box), 24);
	gtk_stack_add_titled(GTK_STACK(stack), box, name, title);
	return box;
}

static GtkWidget *button(const char *text, GCallback cb)
{
	GtkWidget *b = gtk_button_new_with_label(text);

	g_signal_connect(b, "clicked", cb, NULL);
	return b;
}

static int manage(int open_update)
{
	GtkWidget *stack, *side, *box, *p, *row, *scroll, *combo, *vbox;
	char host[64] = "", *version = NULL, *text;

	mg_me = getenv("MY_SETUP_USER");
	if (!mg_me)
		mg_me = "";

	mg_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(mg_win), "Users and system");
	gtk_window_set_default_size(GTK_WINDOW(mg_win), 760, 460);
	gtk_window_set_icon_name(GTK_WINDOW(mg_win), "system-users");
	g_signal_connect(mg_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	stack = gtk_stack_new();

	/* Users */
	p = mg_page(stack, "users", "Users");
	mg_users = gtk_list_box_new();
	g_signal_connect(mg_users, "row-selected", G_CALLBACK(on_user_selected), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), mg_users);
	gtk_box_pack_start(GTK_BOX(p), scroll, TRUE, TRUE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(row), button("Add user…", G_CALLBACK(on_add_user)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), button("Change password…", G_CALLBACK(on_password)), FALSE, FALSE, 0);
	mg_admin = button("Make administrator", G_CALLBACK(on_admin));
	gtk_box_pack_start(GTK_BOX(row), mg_admin, FALSE, FALSE, 0);
	mg_remove = button("Remove user…", G_CALLBACK(on_remove_user));
	gtk_box_pack_end(GTK_BOX(row), mg_remove, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	/* Computer */
	p = mg_page(stack, "computer", "Computer");
	gethostname(host, sizeof host - 1);
	mg_hostname = entry(0);
	gtk_entry_set_text(GTK_ENTRY(mg_hostname), host);
	gtk_box_pack_start(GTK_BOX(p), label("Computer name", NULL), FALSE, FALSE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(row), mg_hostname, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), button("Save", G_CALLBACK(on_save_hostname)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label("Keyboard layout (for everyone, at once)", NULL),
	                   FALSE, FALSE, 8);
	combo = layout_combo();
	g_signal_connect(combo, "changed", G_CALLBACK(on_layout_manage), NULL);
	gtk_widget_set_halign(combo, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), combo, FALSE, FALSE, 0);

	/* Administrator */
	p = mg_page(stack, "admin", "Administrator");
	gtk_box_pack_start(GTK_BOX(p), label("The administrator (root) password is asked "
		"for when you install the system or change things here.", "dim"), FALSE, FALSE, 0);
	mg_root1 = entry(1);
	mg_root2 = entry(1);
	gtk_box_pack_start(GTK_BOX(p), label("New administrator password", NULL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), mg_root1, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label("Password again", NULL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), mg_root2, FALSE, FALSE, 0);
	row = button("Change password", G_CALLBACK(on_save_root));
	gtk_widget_set_halign(row, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	/* System update */
	p = mg_page(stack, "update", "System update");
	g_file_get_contents("/etc/my-distro/version", &version, NULL, NULL);
	text = g_strdup_printf("Installed: my-distro %s", version ? g_strstrip(version) : "?");
	gtk_box_pack_start(GTK_BOX(p), label(text, NULL), FALSE, FALSE, 0);
	g_free(text);
	g_free(version);
	up_status = label("Look for a new version of my-distro. Updates are checked "
		"automatically too: the taskbar shows a button when one is out.", "dim");
	gtk_box_pack_start(GTK_BOX(p), up_status, FALSE, FALSE, 0);
	up_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(up_view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(up_view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(up_view), 8);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(up_view), 6);
	up_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(up_view));
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), up_view);
	gtk_box_pack_start(GTK_BOX(p), scroll, TRUE, TRUE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	up_check = button("Look for updates", G_CALLBACK(on_check));
	up_install = button("Install update", G_CALLBACK(on_install));
	gtk_style_context_add_class(gtk_widget_get_style_context(up_install), "suggested-action");
	gtk_widget_set_sensitive(up_install, FALSE);
	up_restart = button("Restart now", G_CALLBACK(on_restart));
	gtk_widget_set_no_show_all(up_restart, TRUE);
	up_undo = button("Undo last update…", G_CALLBACK(on_undo));
	gtk_widget_set_no_show_all(up_undo, TRUE);
	gtk_box_pack_start(GTK_BOX(row), up_check, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), up_install, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), up_undo, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), up_restart, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	side = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
	gtk_widget_set_size_request(side, 150, -1);

	mg_status = label("", NULL);
	gtk_container_set_border_width(GTK_CONTAINER(mg_status), 12);
	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(vbox), stack, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), mg_status, FALSE, FALSE, 0);

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start(GTK_BOX(box), side, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), vbox, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(mg_win), box);

	fill_users();
	gtk_widget_show_all(mg_win);
	if (open_update) {
		gtk_stack_set_visible_child_name(GTK_STACK(stack), "update");
		on_check(NULL, NULL);
	}
	gtk_main();
	return 0;
}

/* --- Started by a normal user: ask for the root password ---------------------- */

static GtkWidget *auth_dlg, *auth_err;
static int want_update;           /* --update: open on the System update page */

static void ask_root_password(void);

/* my-admin exits with 2 for a wrong password; otherwise it became the
 * "Users and system" window, which has now been closed. */
static void on_admin_done(GPid pid, gint st, gpointer d)
{
	(void)d;
	g_spawn_close_pid(pid);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 2) {
		ask_root_password();
		gtk_label_set_text(GTK_LABEL(auth_err), "Wrong password.");
		return;
	}
	gtk_main_quit();
}

static void on_auth(GtkDialog *d, gint response, gpointer pass)
{
	const char *argv[] = { "/usr/bin/my-admin", want_update ? "--update" : NULL, NULL };
	GPid pid;
	int in;
	char *pw;

	if (response != GTK_RESPONSE_OK) {
		gtk_main_quit();
		return;
	}
	pw = g_strdup_printf("%s\n", text_of(pass));
	gtk_widget_destroy(GTK_WIDGET(d));
	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                              NULL, NULL, &pid, &in, NULL, NULL, NULL)) {
		g_free(pw);
		gtk_main_quit();
		return;
	}
	if (write(in, pw, strlen(pw)) < 0) {
		/* my-admin then reads nothing and says "wrong password" */
	}
	close(in);
	memset(pw, 0, strlen(pw));
	g_free(pw);
	g_child_watch_add(pid, on_admin_done, NULL);
}

static void ask_root_password(void)
{
	GtkWidget *box, *pass;

	auth_dlg = gtk_dialog_new_with_buttons("Users and system", NULL, 0,
		"Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);
	gtk_window_set_icon_name(GTK_WINDOW(auth_dlg), "system-users");
	box = gtk_dialog_get_content_area(GTK_DIALOG(auth_dlg));
	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	gtk_box_pack_start(GTK_BOX(box), label("Changing users and system settings needs "
		"the administrator (root) password.", NULL), FALSE, FALSE, 0);
	pass = entry(1);
	gtk_box_pack_start(GTK_BOX(box), pass, FALSE, FALSE, 0);
	auth_err = label("", "error");
	gtk_box_pack_start(GTK_BOX(box), auth_err, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(auth_dlg), GTK_RESPONSE_OK);
	g_signal_connect(auth_dlg, "response", G_CALLBACK(on_auth), pass);
	gtk_widget_show_all(auth_dlg);
}

int main(int argc, char **argv)
{
	int first = 0, manage_mode = 0;

	for (int i = 1; i < argc; i++) {
		first |= strcmp(argv[i], "--first-boot") == 0;
		manage_mode |= strcmp(argv[i], "--manage") == 0;
		want_update |= strcmp(argv[i], "--update") == 0;
	}
	gtk_init(&argc, &argv);
	if (geteuid() != 0) {
		if (first || manage_mode) {
			fprintf(stderr, "my-setup: %s needs root\n", argv[1]);
			return 1;
		}
		ask_root_password();
		gtk_main();
		return 0;
	}
	if (first)
		return first_boot();
	return manage(want_update);
}
