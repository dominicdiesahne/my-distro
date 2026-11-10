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
 *   - language: /etc/my-distro/language, the system's (each user can pick
 *     their own in Settings); the setup starts again in the new language
 *   - parental controls: /etc/my-distro/parental, one line per user with
 *     the hours they may use the computer and minutes per day; my-login
 *     enforces them
 *   - firmware (0.9.7): /usr/lib/my-distro/firmware asks fwupd and the LVFS;
 *     every update is installed only after asking
 *   - Secure Boot (0.9.7): my-secureboot status, and "prepare" in a terminal
 *     (mokutil asks for the one-time password there)
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
#include "i18n.h"

#define SETUP_DONE    "/etc/my-distro/setup-done"
#define KEYBOARD_CONF "/etc/X11/xorg.conf.d/00-keyboard.conf"
#define DEMO_USER     "dominic"        /* the live system's account (users.table) */
#define FIRST_UID     1000             /* normal users have this uid or higher */

static const struct { const char *id, *label; } layouts[] = {
	{ "de", N_("German") },
	{ "at", N_("German (Austria)") },
	{ "ch", N_("German (Switzerland)") },
	{ "us", N_("English (US)") },
	{ "gb", N_("English (UK)") },
	{ "fr", N_("French") },
	{ "it", N_("Italian") },
	{ "es", N_("Spanish") },
	{ "nl", N_("Dutch") },
	{ "pl", N_("Polish") },
	{ "cz", N_("Czech") },
	{ "hu", N_("Hungarian") },
	{ "ro", N_("Romanian") },
	{ "tr", N_("Turkish") },
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
		return _("That user name belongs to the system. Choose another one.");
	if (pw && must_be_new)
		return _("That user already exists.");
	if (!pw) {
		snprintf(home, sizeof home, "/home/%s", name);
		const char *add[] = { "/usr/sbin/adduser", "-D", "-H", "-h", home,
		                      "-g", fullname, "-s", "/bin/sh", name, NULL };
		const char *skel[] = { "/bin/cp", "-a", "/etc/skel", home, NULL };

		if (run(add) != 0 || !(pw = getpwnam(name)))
			return _("Could not create the user.");
		if (access(home, F_OK) != 0 && run(skel) != 0)
			g_mkdir_with_parents(home, 0755);
	} else {
		snprintf(home, sizeof home, "%s", pw->pw_dir);
		set_field("/etc/passwd", name, 4, fullname);
	}
	uid_t uid = pw->pw_uid;
	gid_t gid = pw->pw_gid;

	if (set_password(name, password) != 0)
		return _("Could not set the password.");
	/* netdev: may choose Wi-Fi networks (wpa_supplicant's control socket),
	 * connect VPNs and use Bluetooth; audio: may use the sound card
	 * (/dev/snd); video: the webcam (/dev/video*) and 3D graphics (/dev/dri);
	 * lpadmin: may add printers (CUPS) */
	const char *netdev[] = { "/usr/sbin/addgroup", name, "netdev", NULL };
	const char *audio[] = { "/usr/sbin/addgroup", name, "audio", NULL };
	const char *video[] = { "/usr/sbin/addgroup", name, "video", NULL };
	const char *lpadmin[] = { "/usr/sbin/addgroup", name, "lpadmin", NULL };

	run(netdev);
	run(audio);
	run(video);
	run(lpadmin);
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
		return _("Could not change the administrator rights.");
	return NULL;
}

/* Removes a normal user and their home folder. */
static const char *remove_user(const char *name)
{
	struct passwd *pw = getpwnam(name);
	char home[300];
	const char *del[] = { "/usr/sbin/deluser", name, NULL };

	if (!pw || pw->pw_uid < FIRST_UID)
		return _("No such user.");
	snprintf(home, sizeof home, "%s", pw->pw_dir);
	if (run(del) != 0)
		return _("Could not remove the user.");
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
		return _("Could not write /etc/hostname.");
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
	return ok ? NULL : _("Could not save the keyboard layout.");
}

/* --- Time zone ------------------------------------------------------------------ */
/*
 * /etc/localtime is a link to the zone's file in /usr/share/zoneinfo (e.g.
 * Europe/Vienna); glibc reads it to turn the clock's UTC into local time.
 * /etc/timezone names the zone too, for programs that read that instead.
 * The list of zones comes from zone1970.tab (one line per zone).
 */
#define ZONEINFO "/usr/share/zoneinfo"

static void current_timezone(char *buf, size_t len)
{
	char link[256];
	ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
	char *p;

	snprintf(buf, len, "Europe/Vienna");
	if (n > 0) {
		link[n] = '\0';
		if ((p = strstr(link, "zoneinfo/")))
			snprintf(buf, len, "%s", p + 9);
	}
}

static int valid_timezone(const char *id)
{
	char path[300];
	struct stat st;

	if (!id || !id[0] || strstr(id, "..") || id[0] == '/')
		return 0;
	for (const char *c = id; *c; c++)
		if (!isalnum((unsigned char)*c) && !strchr("/_+-", *c))
			return 0;
	snprintf(path, sizeof path, "%s/%s", ZONEINFO, id);
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static const char *set_timezone(const char *id)
{
	char target[300], text[80];

	if (!valid_timezone(id))
		return _("Unknown time zone.");
	snprintf(target, sizeof target, "../usr/share/zoneinfo/%s", id);
	unlink("/etc/localtime.new");
	if (symlink(target, "/etc/localtime.new") != 0 ||
	    rename("/etc/localtime.new", "/etc/localtime") != 0)
		return _("Could not set the time zone.");
	snprintf(text, sizeof text, "%s\n", id);
	write_file("/etc/timezone", text, 0644);
	return NULL;
}

/* All zones, sorted ("Europe/Vienna", ...) */
static GPtrArray *zone_list(void)
{
	static GPtrArray *zones;
	char *text = NULL, **lines;

	if (zones)
		return zones;
	zones = g_ptr_array_new_with_free_func(g_free);
	if (g_file_get_contents(ZONEINFO "/zone1970.tab", &text, NULL, NULL)) {
		lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i]; i++) {
			char **f = g_strsplit(lines[i], "\t", 4);

			if (lines[i][0] != '#' && g_strv_length(f) >= 3 && strchr(f[2], '/'))
				g_ptr_array_add(zones, g_strdup(f[2]));
			g_strfreev(f);
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_ptr_array_add(zones, g_strdup("Etc/UTC"));
	return zones;                     /* timezone_widget() sorts it */
}

static int zone_cmp(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int tz_filling;
static GtkWidget *tz_status_label;   /* where "Time zone set." goes, or NULL */

/* The cities of one region: "Europe/Vienna" -> "Vienna", "America/
 * Argentina/Cordoba" -> "Argentina / Cordoba" */
static void fill_cities(GtkWidget *city, const char *region, const char *select)
{
	GPtrArray *zones = zone_list();
	size_t rl = strlen(region);

	tz_filling = 1;
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(city));
	for (guint i = 0; i < zones->len; i++) {
		const char *z = zones->pdata[i];

		if (strncmp(z, region, rl) == 0 && z[rl] == '/') {
			char *shown = g_strdup(z + rl + 1);

			g_strdelimit(shown, "_", ' ');
			if (strchr(shown, '/')) {
				char **parts = g_strsplit(shown, "/", -1);

				g_free(shown);
				shown = g_strjoinv(" / ", parts);
				g_strfreev(parts);
			}
			gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(city), z, shown);
			g_free(shown);
		}
	}
	if (!select || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(city), select))
		gtk_combo_box_set_active(GTK_COMBO_BOX(city), 0);
	tz_filling = 0;
}

static void on_city(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c), *err;

	(void)d;
	if (tz_filling || !id)
		return;
	err = set_timezone(id);
	if (tz_status_label)
		gtk_label_set_text(GTK_LABEL(tz_status_label), err ? err : _("Time zone set."));
}

static void on_region(GtkComboBox *c, gpointer city)
{
	const char *region = gtk_combo_box_get_active_id(c);

	if (!region)
		return;
	fill_cities(city, region, NULL);
	on_city(GTK_COMBO_BOX(city), NULL);        /* the region's first city */
}

/* Two drop-down lists: region (Europe, America, ...) and city. Choosing a
 * city sets the time zone at once. */
static GtkWidget *timezone_widget(void)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *region = gtk_combo_box_text_new(), *city = gtk_combo_box_text_new();
	GPtrArray *zones = zone_list();
	char now[128], last[64] = "", cur_region[64];

	g_ptr_array_sort(zones, zone_cmp);
	current_timezone(now, sizeof now);
	snprintf(cur_region, sizeof cur_region, "%.*s", (int)strcspn(now, "/"), now);
	for (guint i = 0; i < zones->len; i++) {
		const char *z = zones->pdata[i];
		int n = strcspn(z, "/");

		if ((int)strlen(last) != n || strncmp(last, z, n) != 0) {
			snprintf(last, sizeof last, "%.*s", n, z);
			gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(region), last, last);
		}
	}
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(region), cur_region);
	fill_cities(city, cur_region, now);
	g_signal_connect(region, "changed", G_CALLBACK(on_region), city);
	g_signal_connect(city, "changed", G_CALLBACK(on_city), NULL);
	gtk_box_pack_start(GTK_BOX(box), region, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), city, TRUE, TRUE, 0);
	return box;
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

/* --- Language -------------------------------------------------------------------- */

static const struct { const char *id, *label; } languages[] = { LANGUAGES, { NULL, NULL } };

static const char *set_system_language(const char *id)
{
	char text[64];

	snprintf(text, sizeof text, "%s\n", id);
	return write_file(SYSTEM_LANGUAGE, text, 0644) == 0 ? NULL
	       : _("Could not save the language.");
}

static GtkWidget *language_combo(void)
{
	GtkWidget *c = gtk_combo_box_text_new();
	char now[64];

	for (int i = 0; languages[i].id; i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), languages[i].id, languages[i].label);
	i18n_system_language(now, sizeof now);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), now))
		gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
	return c;
}

/* --- Parental controls -------------------------------------------------------- */
/* /etc/my-distro/parental: "NAME FROM-TO MINUTES" per user, e.g.
 * "lena 08:00-20:00 120" ("-" = any time, 0 = no daily limit). my-login
 * reads it (see login/my-login.c). */
#define PARENTAL "/etc/my-distro/parental"

struct limits { int on_hours, from, to, minutes; };   /* from/to: minutes of the day */

static void get_limits(const char *name, struct limits *l)
{
	char *text = NULL, **lines;

	*l = (struct limits){ 0, 8 * 60, 20 * 60, 0 };
	if (!g_file_get_contents(PARENTAL, &text, NULL, NULL))
		return;
	lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char u[64], hours[32];
		int min, h1, m1, h2, m2;

		if (lines[i][0] == '#' || sscanf(lines[i], "%63s %31s %d", u, hours, &min) != 3 ||
		    strcmp(u, name) != 0)
			continue;
		if (sscanf(hours, "%d:%d-%d:%d", &h1, &m1, &h2, &m2) == 4)
			*l = (struct limits){ 1, h1 * 60 + m1, h2 * 60 + m2, 0 };
		l->minutes = min > 0 ? min : 0;
	}
	g_strfreev(lines);
	g_free(text);
}

/* Writes the user's line (or removes it: no limits at all) */
static const char *set_limits(const char *name, const struct limits *l)
{
	char *text = NULL, **lines;
	GString *out = g_string_new("# Parental controls (\"Users and system\"): NAME FROM-TO "
	                            "MINUTES_PER_DAY\n# \"-\" = any time, 0 = no daily limit. "
	                            "my-login enforces them.\n");
	int ok;

	if (g_file_get_contents(PARENTAL, &text, NULL, NULL)) {
		lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i]; i++) {
			char u[64];

			if (!lines[i][0] || lines[i][0] == '#' ||
			    (sscanf(lines[i], "%63s", u) == 1 && strcmp(u, name) == 0))
				continue;
			g_string_append_printf(out, "%s\n", lines[i]);
		}
		g_strfreev(lines);
		g_free(text);
	}
	if (l->on_hours || l->minutes > 0) {
		if (l->on_hours)
			g_string_append_printf(out, "%s %02d:%02d-%02d:%02d %d\n", name, l->from / 60,
			                       l->from % 60, l->to / 60, l->to % 60, l->minutes);
		else
			g_string_append_printf(out, "%s - %d\n", name, l->minutes);
	}
	ok = write_file(PARENTAL, out->str, 0644) == 0;
	g_string_free(out, TRUE);
	return ok ? NULL : _("Could not save the parental controls.");
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
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), layouts[i].id, _(layouts[i].label));
	current_layout(now, sizeof now);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), now)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), now, now);  /* set by hand */
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), now);
	}
	return c;
}

/* First boot: a language was chosen. Save it and start the setup again in
 * that language (GTK reads the translations only at the start). */
static char **fb_argv;

static void on_language_first(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (!id || set_system_language(id))
		return;
	setenv("LANG", id, 1);
	execv("/proc/self/exe", fb_argv);
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
		return _("Choose a password.");
	if (strcmp(text_of(a), text_of(b)) != 0)
		return _("The two passwords are not the same.");
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
	char wall[512], image[540], *css;

	memset(&s, 0, sizeof s);
	settings_load(&s, "/nonexistent");          /* system defaults only */
	for (int i = 0; themes[i].name; i++)
		if (strcmp(themes[i].name, fb_theme) == 0)
			s.pal = themes[i];
	g_strlcpy(s.theme, fb_theme, sizeof s.theme);
	strcpy(s.wallpaper, "theme");
	settings_wallpaper_file(&s, wall, sizeof wall);
	/* high-contrast themes have no picture */
	if (wall[0])
		snprintf(image, sizeof image, "url('%s')", wall);
	else
		strcpy(image, "none");
	g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme",
	             s.pal.dark, NULL);
	css = g_strdup_printf(
		"window.setup { background-color: %s; background-image: %s;"
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
		err = _("User name: small letters a-z, digits, - and _, starting with a letter.");
	else if ((pw = getpwnam(user)) && pw->pw_uid < FIRST_UID)
		err = _("That user name belongs to the system. Choose another one.");
	else if (!valid_fullname(text_of(fb_fullname)))
		err = _("The name can't contain \":\".");
	else
		err = check_passwords(fb_pass1, fb_pass2);
	if (!err && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fb_same_root)) &&
	    check_passwords(fb_root1, fb_root2))
		err = _("Administrator password: type the same password twice.");
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
		show_error(fb_err_computer, _("Computer name: letters, digits and -, no spaces."));
		return;
	}
	fullname = g_strdup(text_of(fb_fullname)[0] ? text_of(fb_fullname) : user);
	err = save_user(user, fullname, text_of(fb_pass1), fb_theme, 0);
	g_free(fullname);
	if (!err && set_password("root", root_pw) != 0)
		err = _("Could not set the administrator password.");
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

	msg = g_strdup_printf(_("Your account \"%s\" is ready. Log in with your new password."),
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
	gtk_window_set_title(GTK_WINDOW(win), _("Setup"));
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
	p = fb_page("welcome", _("Welcome to my-distro"),
	            _("Let's set up your computer: your keyboard, your own account and a "
	            "name for the computer. It takes a minute, and you can change all of "
	            "it later in \"Users and system\"."));
	row = language_combo();
	g_signal_connect(row, "changed", G_CALLBACK(on_language_first), NULL);
	add_field(p, "Language / Sprache", row);
	row = layout_combo();
	g_signal_connect(row, "changed", G_CALLBACK(on_layout), NULL);
	add_field(p, _("Keyboard layout"), row);
	add_field(p, _("Try your keyboard here"), entry(0));
	add_field(p, _("Time zone"), timezone_widget());
	if (live) {
		GtkWidget *skip = gtk_button_new_with_label(_("Skip: try it with the demo account"));

		gtk_widget_set_halign(skip, GTK_ALIGN_START);
		gtk_widget_set_margin_top(skip, 14);
		gtk_button_set_relief(GTK_BUTTON(skip), GTK_RELIEF_NONE);
		g_signal_connect(skip, "clicked", G_CALLBACK(on_skip), NULL);
		gtk_box_pack_start(GTK_BOX(p), skip, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(p),
			label(_("The live system keeps nothing after a reboot. Install it to "
			      "keep your account."), "dim"), FALSE, FALSE, 0);
	}
	set_next(p, buttons(NULL, NULL, NULL, _("Next"), G_CALLBACK(on_page), "account"));

	/* 2. Account */
	p = fb_page("account", _("Your account"), NULL);
	fb_fullname = entry(0);
	fb_username = entry(0);
	fb_pass1 = entry(1);
	fb_pass2 = entry(1);
	g_signal_connect(fb_fullname, "changed", G_CALLBACK(on_fullname), NULL);
	g_signal_connect(fb_username, "changed", G_CALLBACK(on_username), NULL);
	add_field(p, _("Your name"), fb_fullname);
	add_field(p, _("User name (to log in)"), fb_username);
	add_field(p, _("Password"), fb_pass1);
	add_field(p, _("Password again"), fb_pass2);
	fb_same_root = gtk_check_button_new_with_label(
		_("Use this password for the administrator (root) too"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fb_same_root), TRUE);
	gtk_widget_set_margin_top(fb_same_root, 10);
	g_signal_connect(fb_same_root, "toggled", G_CALLBACK(on_same_root), NULL);
	gtk_box_pack_start(GTK_BOX(p), fb_same_root, FALSE, FALSE, 0);
	fb_root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	fb_root1 = entry(1);
	fb_root2 = entry(1);
	add_field(fb_root_box, _("Administrator password"), fb_root1);
	add_field(fb_root_box, _("Administrator password again"), fb_root2);
	gtk_box_pack_start(GTK_BOX(p), fb_root_box, FALSE, FALSE, 0);
	/* hidden until "same password" is unticked; show_all below skips it */
	gtk_widget_show_all(fb_root_box);
	gtk_widget_hide(fb_root_box);
	gtk_widget_set_no_show_all(fb_root_box, TRUE);
	fb_err_account = label("", "error");
	gtk_box_pack_start(GTK_BOX(p), fb_err_account, FALSE, FALSE, 4);
	set_next(p, buttons(_("Back"), G_CALLBACK(on_page), "welcome",
	                    _("Next"), G_CALLBACK(on_account_next), NULL));

	/* 3. Computer name and theme */
	p = fb_page("computer", _("Your computer"), NULL);
	fb_hostname = entry(0);
	gethostname(host, sizeof host - 1);
	gtk_entry_set_text(GTK_ENTRY(fb_hostname), host);
	add_field(p, _("Computer name (shown on the network and the login screen)"), fb_hostname);
	flow = gtk_flow_box_new();
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 3);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 3);
	for (int i = 0; themes[i].name; i++) {
		GtkWidget *b = gtk_radio_button_new_with_label(group, _(themes[i].label));

		group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(b));
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);
		gtk_style_context_add_class(gtk_widget_get_style_context(b), "theme");
		g_signal_connect(b, "toggled", G_CALLBACK(on_theme_button), (gpointer)themes[i].name);
		gtk_container_add(GTK_CONTAINER(flow), b);
	}
	add_field(p, _("Colors (more choices later in Settings)"), flow);
	fb_err_computer = label("", "error");
	gtk_box_pack_start(GTK_BOX(p), fb_err_computer, FALSE, FALSE, 4);
	set_next(p, buttons(_("Back"), G_CALLBACK(on_page), "account",
	                    _("Finish"), G_CALLBACK(on_finish), NULL));

	/* 4. Done */
	p = fb_page("done", _("All set"), NULL);
	fb_done_text = label("", NULL);
	gtk_box_pack_start(GTK_BOX(p), fb_done_text, FALSE, FALSE, 0);
	set_next(p, buttons(NULL, NULL, NULL, _("Go to the login screen"),
	                    G_CALLBACK(gtk_main_quit), NULL));

	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(win);
	gtk_window_present(GTK_WINDOW(win));   /* no window manager: takes the focus */
	on_page_shown(G_OBJECT(fb_stack), NULL, NULL);
	gtk_main();
	return 0;
}

/* --- "Users and system" ------------------------------------------------------------ */

static GtkWidget *mg_admin, *mg_parental;
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
	gtk_widget_set_sensitive(mg_parental, name != NULL);
	gtk_button_set_label(GTK_BUTTON(mg_admin), name && is_admin(name)
	                     ? _("Remove administrator rights") : _("Make administrator"));
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
	gtk_widget_set_sensitive(mg_parental, FALSE);
	gtk_button_set_label(GTK_BUTTON(mg_admin), _("Make administrator"));
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
	       ? _("Administrator now: may use sudo with their own password.")
	       : _("No administrator rights any more."));
	fill_users();
	g_free(name);
}

/* A dialog with text fields; returns TRUE when OK was pressed. */
static GtkWidget *form_dialog(const char *title, GtkWidget **grid)
{
	GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(mg_win),
		GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("OK"), GTK_RESPONSE_OK, NULL);

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
	GtkWidget *grid, *dlg = form_dialog(_("Add a user"), &grid);
	GtkWidget *full = entry(0), *name = entry(0), *p1 = entry(1), *p2 = entry(1);
	GtkWidget *err = label("", "error");

	(void)b; (void)d;
	grid_row(grid, 0, _("Full name"), full);
	grid_row(grid, 1, _("User name"), name);
	grid_row(grid, 2, _("Password"), p1);
	grid_row(grid, 3, _("Password again"), p2);
	gtk_grid_attach(GTK_GRID(grid), err, 0, 4, 2, 1);
	gtk_widget_show_all(dlg);
	while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
		const char *e = NULL;

		if (!valid_username(text_of(name)))
			e = _("User name: small letters a-z, digits, - and _, starting with a letter.");
		else if (!valid_fullname(text_of(full)))
			e = _("The full name can't contain \":\".");
		else if (!(e = check_passwords(p1, p2)))
			e = save_user(text_of(name), text_of(full)[0] ? text_of(full) : text_of(name),
			              text_of(p1), NULL, 1);
		if (!e) {
			status(NULL, _("User added."));
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
		status(_("Choose a user in the list first."), NULL);
		return;
	}
	title = g_strdup_printf(_("New password for %s"), user);
	dlg = form_dialog(title, &grid);
	g_free(title);
	grid_row(grid, 0, _("New password"), p1);
	grid_row(grid, 1, _("Password again"), p2);
	gtk_grid_attach(GTK_GRID(grid), err, 0, 2, 2, 1);
	gtk_widget_show_all(dlg);
	while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
		const char *e = check_passwords(p1, p2);

		if (!e && set_password(user, text_of(p1)) != 0)
			e = _("Could not set the password.");
		if (!e) {
			status(NULL, _("Password changed."));
			break;
		}
		gtk_label_set_text(GTK_LABEL(err), e);
	}
	gtk_widget_destroy(dlg);
}

/* "08:00" -> minutes of the day, -1 if it isn't a time */
static int parse_time(const char *s)
{
	int h, m;
	char end;

	if (sscanf(s, "%d:%d%c", &h, &m, &end) != 2 || h < 0 || h > 24 || m < 0 || m > 59 ||
	    (h == 24 && m > 0))
		return -1;
	return h * 60 + m;
}

static void on_limits_toggled(GtkToggleButton *t, gpointer w)
{
	gtk_widget_set_sensitive(w, gtk_toggle_button_get_active(t));
}

/* Parental controls of the chosen user: when and how long they may use
 * the computer */
static void on_parental(GtkButton *b, gpointer d)
{
	const char *user = selected_user();
	GtkWidget *grid, *dlg, *hours, *times, *from, *to, *daily, *mins, *err, *note;
	struct limits l;
	char t[16], *title;

	(void)b; (void)d;
	if (!user) {
		status(_("Choose a user in the list first."), NULL);
		return;
	}
	get_limits(user, &l);
	title = g_strdup_printf(_("Parental controls for %s"), user);
	dlg = form_dialog(title, &grid);
	g_free(title);

	hours = gtk_check_button_new_with_label(_("May use the computer only from"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hours), l.on_hours);
	times = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	from = entry(0);
	to = entry(0);
	gtk_entry_set_width_chars(GTK_ENTRY(from), 6);
	gtk_entry_set_width_chars(GTK_ENTRY(to), 6);
	snprintf(t, sizeof t, "%02d:%02d", l.from / 60, l.from % 60);
	gtk_entry_set_text(GTK_ENTRY(from), t);
	snprintf(t, sizeof t, "%02d:%02d", l.to / 60, l.to % 60);
	gtk_entry_set_text(GTK_ENTRY(to), t);
	gtk_box_pack_start(GTK_BOX(times), from, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(times), gtk_label_new(_("to")), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(times), to, FALSE, FALSE, 0);
	gtk_widget_set_sensitive(times, l.on_hours);
	g_signal_connect(hours, "toggled", G_CALLBACK(on_limits_toggled), times);
	gtk_grid_attach(GTK_GRID(grid), hours, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), times, 1, 0, 1, 1);

	daily = gtk_check_button_new_with_label(_("At most, minutes per day"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(daily), l.minutes > 0);
	mins = gtk_spin_button_new_with_range(15, 960, 15);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(mins), l.minutes > 0 ? l.minutes : 120);
	gtk_widget_set_sensitive(mins, l.minutes > 0);
	g_signal_connect(daily, "toggled", G_CALLBACK(on_limits_toggled), mins);
	gtk_grid_attach(GTK_GRID(grid), daily, 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), mins, 1, 1, 1, 1);

	note = label(_("The login screen checks this. 5 minutes before the time is over a "
		"notice appears, then the session ends (unsaved work is lost)."), "dim");
	gtk_grid_attach(GTK_GRID(grid), note, 0, 2, 2, 1);
	err = label("", "error");
	gtk_grid_attach(GTK_GRID(grid), err, 0, 3, 2, 1);
	gtk_widget_show_all(dlg);
	while (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
		struct limits n = { gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hours)),
		                    parse_time(text_of(from)), parse_time(text_of(to)),
		                    gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(daily))
		                    ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(mins)) : 0 };
		const char *e = NULL;

		if (n.on_hours && (n.from < 0 || n.to < 0))
			e = _("Write the times like 08:00 and 20:00.");
		else if (!(e = set_limits(user, &n))) {
			status(NULL, n.on_hours || n.minutes ? _("Parental controls saved.")
			                                     : _("No limits for this user."));
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
		_("Remove the user \"%s\"?"), user);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask),
		_("Their home folder /home/%s and all files in it are deleted."), user);
	gtk_dialog_add_buttons(GTK_DIALOG(ask), _("Cancel"), GTK_RESPONSE_CANCEL,
	                       _("Remove"), GTK_RESPONSE_ACCEPT, NULL);
	if (gtk_dialog_run(GTK_DIALOG(ask)) == GTK_RESPONSE_ACCEPT) {
		status(remove_user(user), _("User removed."));
		fill_users();
	}
	gtk_widget_destroy(ask);
}

static void on_save_hostname(GtkButton *b, gpointer d)
{
	const char *h = text_of(mg_hostname);

	(void)b; (void)d;
	if (!valid_hostname(h))
		status(_("Computer name: letters, digits and -, no spaces."), NULL);
	else
		status(set_hostname(h), _("Computer name saved."));
}

static void on_layout_manage(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (id)
		status(set_layout(id), _("Keyboard layout changed."));
}

static void on_language_manage(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (id)
		status(set_system_language(id), _("Language saved: it is used from the next login."));
}

static void on_save_root(GtkButton *b, gpointer d)
{
	const char *e = check_passwords(mg_root1, mg_root2);

	(void)b; (void)d;
	if (!e && set_password("root", text_of(mg_root1)) != 0)
		e = _("Could not set the password.");
	status(e, _("Administrator password changed."));
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
	gtk_label_set_text(GTK_LABEL(up_status), _("Looking for updates…"));
	gtk_widget_set_sensitive(up_install, FALSE);
	while (gtk_events_pending())
		gtk_main_iteration();
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, &st, NULL) || !out) {
		gtk_label_set_text(GTK_LABEL(up_status), _("my-update is missing."));
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
		char *t = g_strdup_printf(_("Undo last update (back to %s)…"), up_previous);

		gtk_button_set_label(GTK_BUTTON(up_undo), t);
		g_free(t);
		gtk_widget_show(up_undo);
	} else {
		gtk_widget_hide(up_undo);
	}
	notes = strstr(out, "notes:\n");
	gtk_text_buffer_set_text(up_buf, notes ? notes + 7 : "", -1);

	if (err[0])
		msg = g_strdup_printf(_("Could not look for updates (no internet?): %s"), err);
	else if (strcmp(available, "1") != 0)
		msg = g_strdup_printf(_("my-distro %s is up to date."), installed);
	else if (strcmp(live, "1") == 0)
		msg = g_strdup_printf(_("my-distro %s is out. This is the live system, which "
			"forgets updates: download the new ISO from GitHub."), up_latest);
	else if (strcmp(installable, "1") != 0)
		msg = g_strdup_printf(_("my-distro %s is out, but without an update package: "
			"download its ISO from GitHub."), up_latest);
	else
		msg = g_strdup_printf(_("my-distro %s is ready to install (you have %s). "
			"What's new:"), up_latest, installed);
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
			? _("The previous version is back. Restart the computer to finish.")
			: _("The update is installed. Restart the computer to finish. "
			  "If something doesn't work afterwards, \"Undo last update\" goes back."));
		gtk_widget_show(up_restart);
		if (up_undoing)
			gtk_widget_hide(up_undo);
	} else {
		gtk_label_set_text(GTK_LABEL(up_status), up_undoing
			? _("Going back did not work: see the messages below.")
			: _("The update did not work: see the messages below."));
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
	gtk_dialog_add_buttons(GTK_DIALOG(ask), _("Cancel"), GTK_RESPONSE_CANCEL,
	                       yes, GTK_RESPONSE_ACCEPT, NULL);
	answer = gtk_dialog_run(GTK_DIALOG(ask));
	gtk_widget_destroy(ask);
	if (answer != GTK_RESPONSE_ACCEPT)
		return;

	up_undoing = strcmp(cmd, "rollback") == 0;
	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL,
	                              G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
	                              NULL, NULL, &pid, NULL, &out, NULL, NULL)) {
		gtk_label_set_text(GTK_LABEL(up_status), _("Could not start my-update."));
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
	char *q = g_strdup_printf(_("Install my-distro %s?"), up_latest);

	(void)b; (void)d;
	run_update("install", q, _("Your files, users and settings stay. Don't switch the "
	           "computer off while it installs; restart it afterwards."), _("Install"),
	           _("Installing… don't switch the computer off."));
	g_free(q);
}

static void on_undo(GtkButton *b, gpointer d)
{
	char *q = g_strdup_printf(_("Go back to my-distro %s?"), up_previous);

	(void)b; (void)d;
	run_update("rollback", q, _("The system files of the version before the last update "
	           "come back. Your files, users and settings stay. Restart afterwards."),
	           _("Go back"), _("Going back… don't switch the computer off."));
	g_free(q);
}

static void on_restart(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	kill(1, SIGTERM);                 /* our init: TERM = reboot */
}

/* Factory reset: my-reset marks it for the next start, then we restart.
 * It runs at the start (S00reset) before anything has the files open. */
static void on_reset(GtkButton *b, gpointer d)
{
	const char *argv[] = { "/usr/sbin/my-reset", "--schedule", NULL };
	GtkWidget *ask, *yes;
	int answer, st = 1;

	(void)b; (void)d;
	ask = gtk_message_dialog_new(GTK_WINDOW(mg_win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
		GTK_BUTTONS_NONE, _("Delete all users, files and settings?"));
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask),
		_("Every user and everything in their home folders (documents, pictures, browser "
		"data) is deleted, and all settings: passwords, computer name, Wi-Fi networks, "
		"VPNs, time zone. Only the system stays. This can't be undone.\n\n"
		"The computer restarts now; the reset takes a few seconds, then the setup "
		"starts as after installing."));
	gtk_dialog_add_button(GTK_DIALOG(ask), _("Cancel"), GTK_RESPONSE_CANCEL);
	yes = gtk_dialog_add_button(GTK_DIALOG(ask), _("Reset and restart"), GTK_RESPONSE_ACCEPT);
	gtk_style_context_add_class(gtk_widget_get_style_context(yes), "destructive-action");
	gtk_dialog_set_default_response(GTK_DIALOG(ask), GTK_RESPONSE_CANCEL);
	answer = gtk_dialog_run(GTK_DIALOG(ask));
	gtk_widget_destroy(ask);
	if (answer != GTK_RESPONSE_ACCEPT)
		return;
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL |
	                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &st, NULL) ||
	    !g_spawn_check_wait_status(st, NULL)) {
		status(_("Could not start the factory reset."), NULL);
		return;
	}
	kill(1, SIGTERM);                 /* our init: TERM = reboot */
}

/* --- Firmware updates page (0.9.7): /usr/lib/my-distro/firmware, fwupd -------- */

static GtkWidget *fw_status, *fw_list, *fw_check, *fw_view, *fw_restart;
static GtkTextBuffer *fw_buf;
static GString *fw_out;           /* what the running helper printed so far */
static int fw_installing;

static gboolean on_fw_output(GIOChannel *ch, GIOCondition cond, gpointer d)
{
	char *line;
	gsize n;

	(void)d;
	if (cond & G_IO_IN)
		while (g_io_channel_read_line(ch, &line, &n, NULL, NULL) == G_IO_STATUS_NORMAL) {
			g_string_append(fw_out, line);
			if (fw_installing) {
				GtkTextIter end;

				gtk_text_buffer_get_end_iter(fw_buf, &end);
				gtk_text_buffer_insert(fw_buf, &end, line, -1);
			}
			g_free(line);
		}
	return !(cond & (G_IO_HUP | G_IO_ERR));
}

static void on_fw_install(GtkButton *b, gpointer d);

/* The helper's "update" lines become rows with an Install button */
static void fw_show_updates(void)
{
	char **lines = g_strsplit(fw_out->str, "\n", -1);
	GList *old = gtk_container_get_children(GTK_CONTAINER(fw_list));
	int n = 0;

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
	for (int i = 0; lines[i]; i++) {
		char **f = g_strsplit(lines[i], "\t", 6);

		if (g_strv_length(f) == 6 && !strcmp(f[0], "update")) {
			GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
			char *t = g_strdup_printf("%s: %s → %s\n%s", f[4], f[2], f[3], f[5]);
			GtkWidget *l = label(t, NULL), *btn = gtk_button_new_with_label(_("Install…"));

			g_object_set_data_full(G_OBJECT(btn), "device", g_strdup(f[1]), g_free);
			g_object_set_data_full(G_OBJECT(btn), "what", g_strdup_printf("%s %s", f[4], f[3]), g_free);
			g_signal_connect(btn, "clicked", G_CALLBACK(on_fw_install), NULL);
			g_object_set(row, "margin", 6, NULL);
			gtk_box_pack_start(GTK_BOX(row), l, TRUE, TRUE, 0);
			gtk_box_pack_end(GTK_BOX(row), btn, FALSE, FALSE, 0);
			gtk_list_box_insert(GTK_LIST_BOX(fw_list), row, -1);
			g_free(t);
			n++;
		} else if (g_strv_length(f) >= 2 && !strcmp(f[0], "error")) {
			char *t = g_strdup_printf(_("Could not look for firmware updates (no internet?): %s"), f[1]);

			gtk_label_set_text(GTK_LABEL(fw_status), t);
			g_free(t);
			n = -1;
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	if (n == 0)
		gtk_label_set_text(GTK_LABEL(fw_status), _("No firmware updates for this computer."));
	else if (n > 0)
		gtk_label_set_text(GTK_LABEL(fw_status), _("Firmware updates for this computer:"));
	gtk_widget_show_all(fw_list);
}

static void on_fw_done(GPid pid, gint st, gpointer d)
{
	(void)d;
	g_spawn_close_pid(pid);
	gtk_widget_set_sensitive(fw_check, TRUE);
	gtk_widget_set_sensitive(fw_list, TRUE);
	if (!fw_installing) {
		fw_show_updates();
	} else if (g_spawn_check_wait_status(st, NULL)) {
		if (strstr(fw_out->str, "\nreboot")) {
			gtk_label_set_text(GTK_LABEL(fw_status), _("The firmware update is ready: it is "
				"installed while the computer restarts. Keep it plugged in."));
			gtk_widget_show(fw_restart);
		} else {
			gtk_label_set_text(GTK_LABEL(fw_status), _("The firmware update is installed."));
		}
	} else {
		gtk_label_set_text(GTK_LABEL(fw_status), _("The firmware update did not work: see the "
			"messages below."));
	}
	fw_installing = 0;
}

static void fw_run(const char **argv)
{
	GPid pid;
	int out;
	GIOChannel *ch;

	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL,
	                              G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
	                              NULL, NULL, &pid, NULL, &out, NULL, NULL)) {
		gtk_label_set_text(GTK_LABEL(fw_status), _("fwupd is missing."));
		return;
	}
	g_string_truncate(fw_out, 0);
	gtk_widget_set_sensitive(fw_check, FALSE);
	gtk_widget_set_sensitive(fw_list, FALSE);
	ch = g_io_channel_unix_new(out);
	g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_fw_output, NULL);
	g_io_channel_unref(ch);
	g_child_watch_add(pid, on_fw_done, NULL);
}

static void on_fw_check(GtkButton *b, gpointer d)
{
	const char *argv[] = { "/usr/lib/my-distro/firmware", "check", NULL };

	(void)b; (void)d;
	fw_installing = 0;
	gtk_label_set_text(GTK_LABEL(fw_status), _("Asking the LVFS (fwupd.org) for firmware "
		"updates…"));
	fw_run(argv);
}

/* Firmware updates always ask first: a wrong one, or switching off while
 * it is written, can leave a part (or the whole computer) unusable */
static void on_fw_install(GtkButton *b, gpointer d)
{
	const char *argv[] = { "/usr/lib/my-distro/firmware", "install",
	                       g_object_get_data(G_OBJECT(b), "device"), NULL };
	GtkWidget *ask;
	int answer;

	(void)d;
	ask = gtk_message_dialog_new(GTK_WINDOW(mg_win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
		GTK_BUTTONS_NONE, _("Install the firmware update %s?"),
		(char *)g_object_get_data(G_OBJECT(b), "what"));
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask),
		_("It comes from the part's maker through the LVFS. Laptops: plug in the power. "
		"Don't switch the computer off until it is done; some updates finish while the "
		"computer restarts."));
	gtk_dialog_add_buttons(GTK_DIALOG(ask), _("Cancel"), GTK_RESPONSE_CANCEL,
	                       _("Install"), GTK_RESPONSE_ACCEPT, NULL);
	answer = gtk_dialog_run(GTK_DIALOG(ask));
	gtk_widget_destroy(ask);
	if (answer != GTK_RESPONSE_ACCEPT)
		return;
	fw_installing = 1;
	gtk_text_buffer_set_text(fw_buf, "", -1);
	gtk_label_set_text(GTK_LABEL(fw_status), _("Installing the firmware update… don't "
		"switch the computer off."));
	fw_run(argv);
}

/* --- Secure Boot page (0.9.7): my-secureboot ---------------------------------- */

static GtkWidget *sb_label;

static void sb_fill(void)
{
	const char *argv[] = { "/usr/sbin/my-secureboot", "status", NULL };
	char *out = NULL, state[32] = "", enrolled[32] = "", inst[32] = "", lock[32] = "";
	GString *t = g_string_new(NULL);

	/* status prints "key = value" lines */
	if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
	                 &out, NULL, NULL, NULL) && out) {
		char **lines = g_strsplit(out, "\n", -1);

		for (int i = 0; lines[i]; i++) {
			char *eq = strstr(lines[i], " = ");

			if (!eq)
				continue;
			*eq = '\0';
			if (!strcmp(lines[i], "secureboot")) g_strlcpy(state, eq + 3, sizeof state);
			if (!strcmp(lines[i], "enrolled")) g_strlcpy(enrolled, eq + 3, sizeof enrolled);
			if (!strcmp(lines[i], "installation")) g_strlcpy(inst, eq + 3, sizeof inst);
			if (!strcmp(lines[i], "lockdown")) g_strlcpy(lock, eq + 3, sizeof lock);
		}
		g_strfreev(lines);
	}
	if (!strcmp(state, "unsupported"))
		g_string_append(t, _("This computer was started without UEFI: Secure Boot doesn't apply."));
	else {
		g_string_append(t, !strcmp(state, "on") ? _("Secure Boot is on.") : _("Secure Boot is off."));
		g_string_append(t, " ");
		g_string_append(t, !strcmp(enrolled, "yes") ? _("my-distro's key is enrolled.") :
		                   !strcmp(enrolled, "next start") ? _("my-distro's key is enrolled at the next start (MokManager).") :
		                   _("my-distro's key is not enrolled yet."));
		if (!strcmp(inst, "grub")) {
			g_string_append(t, " ");
			g_string_append(t, _("This installation doesn't start through shim yet."));
		}
		if (lock[0] && strcmp(lock, "none")) {
			g_string_append(t, " ");
			g_string_append(t, _("The kernel is locked down: hibernation is off."));
		}
	}
	gtk_label_set_text(GTK_LABEL(sb_label), t->str);
	g_string_free(t, TRUE);
	g_free(out);
}

static void on_sb_prepare(GtkButton *b, gpointer d)
{
	const char *argv[] = { "xterm", "-T", "Secure Boot", "-hold", "-e",
	                       "/usr/sbin/my-secureboot", "prepare", NULL };
	int st;

	(void)b; (void)d;
	g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &st, NULL);
	sb_fill();
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
	gtk_window_set_title(GTK_WINDOW(mg_win), _("Users and system"));
	gtk_window_set_default_size(GTK_WINDOW(mg_win), 760, 460);
	gtk_window_set_icon_name(GTK_WINDOW(mg_win), "system-users");
	g_signal_connect(mg_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	stack = gtk_stack_new();

	/* Users */
	p = mg_page(stack, "users", _("Users"));
	mg_users = gtk_list_box_new();
	g_signal_connect(mg_users, "row-selected", G_CALLBACK(on_user_selected), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), mg_users);
	gtk_box_pack_start(GTK_BOX(p), scroll, TRUE, TRUE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(row), button(_("Add user…"), G_CALLBACK(on_add_user)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), button(_("Change password…"), G_CALLBACK(on_password)), FALSE, FALSE, 0);
	mg_admin = button(_("Make administrator"), G_CALLBACK(on_admin));
	gtk_box_pack_start(GTK_BOX(row), mg_admin, FALSE, FALSE, 0);
	mg_parental = button(_("Parental controls…"), G_CALLBACK(on_parental));
	gtk_box_pack_start(GTK_BOX(row), mg_parental, FALSE, FALSE, 0);
	mg_remove = button(_("Remove user…"), G_CALLBACK(on_remove_user));
	gtk_box_pack_end(GTK_BOX(row), mg_remove, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	/* Computer */
	p = mg_page(stack, "computer", _("Computer"));
	gethostname(host, sizeof host - 1);
	mg_hostname = entry(0);
	gtk_entry_set_text(GTK_ENTRY(mg_hostname), host);
	gtk_box_pack_start(GTK_BOX(p), label(_("Computer name"), NULL), FALSE, FALSE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(row), mg_hostname, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), button(_("Save"), G_CALLBACK(on_save_hostname)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label(_("Keyboard layout (for everyone, at once)"), NULL),
	                   FALSE, FALSE, 8);
	combo = layout_combo();
	g_signal_connect(combo, "changed", G_CALLBACK(on_layout_manage), NULL);
	gtk_widget_set_halign(combo, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label(_("Time zone (for everyone, at once)"), NULL),
	                   FALSE, FALSE, 8);
	combo = timezone_widget();
	gtk_widget_set_halign(combo, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label(_("Language (for everyone who didn't choose "
		"their own in Settings; at the next login)"), NULL), FALSE, FALSE, 8);
	combo = language_combo();
	g_signal_connect(combo, "changed", G_CALLBACK(on_language_manage), NULL);
	gtk_widget_set_halign(combo, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), combo, FALSE, FALSE, 0);

	/* Administrator */
	p = mg_page(stack, "admin", _("Administrator"));
	gtk_box_pack_start(GTK_BOX(p), label(_("The administrator (root) password is asked "
		"for when you install the system or change things here."), "dim"), FALSE, FALSE, 0);
	mg_root1 = entry(1);
	mg_root2 = entry(1);
	gtk_box_pack_start(GTK_BOX(p), label(_("New administrator password"), NULL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), mg_root1, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label(_("Password again"), NULL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), mg_root2, FALSE, FALSE, 0);
	row = button(_("Change password"), G_CALLBACK(on_save_root));
	gtk_widget_set_halign(row, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	/* System update */
	p = mg_page(stack, "update", _("System update"));
	g_file_get_contents("/etc/my-distro/version", &version, NULL, NULL);
	text = g_strdup_printf(_("Installed: my-distro %s"), version ? g_strstrip(version) : "?");
	gtk_box_pack_start(GTK_BOX(p), label(text, NULL), FALSE, FALSE, 0);
	g_free(text);
	g_free(version);
	up_status = label(_("Look for a new version of my-distro. Updates are checked "
		"automatically too: the taskbar shows a button when one is out."), "dim");
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
	up_check = button(_("Look for updates"), G_CALLBACK(on_check));
	up_install = button(_("Install update"), G_CALLBACK(on_install));
	gtk_style_context_add_class(gtk_widget_get_style_context(up_install), "suggested-action");
	gtk_widget_set_sensitive(up_install, FALSE);
	up_restart = button(_("Restart now"), G_CALLBACK(on_restart));
	gtk_widget_set_no_show_all(up_restart, TRUE);
	up_undo = button(_("Undo last update…"), G_CALLBACK(on_undo));
	gtk_widget_set_no_show_all(up_undo, TRUE);
	gtk_box_pack_start(GTK_BOX(row), up_check, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), up_install, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), up_undo, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), up_restart, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	/* Firmware updates (0.9.7) */
	p = mg_page(stack, "firmware", _("Firmware"));
	fw_out = g_string_new(NULL);
	fw_status = label(_("Firmware is the software inside the computer's parts (UEFI, disks, "
		"docks, mice, ...). fwupd asks the LVFS (fwupd.org) for updates from their makers. "
		"Nothing is installed without asking."), "dim");
	gtk_box_pack_start(GTK_BOX(p), fw_status, FALSE, FALSE, 0);
	fw_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(fw_list), GTK_SELECTION_NONE);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), fw_list);
	gtk_box_pack_start(GTK_BOX(p), scroll, TRUE, TRUE, 0);
	fw_view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(fw_view), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(fw_view), GTK_WRAP_WORD_CHAR);
	fw_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(fw_view));
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_widget_set_size_request(scroll, -1, 90);
	gtk_container_add(GTK_CONTAINER(scroll), fw_view);
	gtk_box_pack_start(GTK_BOX(p), scroll, FALSE, FALSE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	fw_check = button(_("Look for firmware updates"), G_CALLBACK(on_fw_check));
	fw_restart = button(_("Restart now"), G_CALLBACK(on_restart));
	gtk_widget_set_no_show_all(fw_restart, TRUE);
	gtk_box_pack_start(GTK_BOX(row), fw_check, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), fw_restart, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 0);

	/* Secure Boot (0.9.7) */
	p = mg_page(stack, "secureboot", _("Secure Boot"));
	sb_label = label("", NULL);
	gtk_box_pack_start(GTK_BOX(p), sb_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label(_("my-distro starts with Secure Boot on once its key "
		"is enrolled in the PC (once; you need the computer's screen and keyboard for it). "
		"\"Prepare\" asks for a one-time password; at the next start a blue screen "
		"(MokManager) appears: choose \"Enroll MOK\", \"Continue\", \"Yes\", type the "
		"password, \"Reboot\". Then switch on Secure Boot in the firmware settings."), "dim"),
		FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label(_("With Secure Boot on, the computer can't "
		"hibernate (the kernel locks itself down and can't check a saved memory image). "
		"Suspend works."), "dim"), FALSE, FALSE, 0);
	row = button(_("Prepare for Secure Boot…"), G_CALLBACK(on_sb_prepare));
	gtk_widget_set_halign(row, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 8);
	sb_fill();

	/* Factory reset */
	p = mg_page(stack, "reset", _("Factory reset"));
	gtk_box_pack_start(GTK_BOX(p), label(_("Puts this computer back to how it was right after "
		"installing my-distro: all users, their files and all settings are deleted. "
		"The system itself stays, in the version it has now. Then the setup starts."),
		NULL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(p), label(_("If the desktop doesn't start any more: choose "
		"\"recovery mode\" in the boot menu and run my-reset there."), "dim"), FALSE, FALSE, 0);
	row = button(_("Reset this computer…"), G_CALLBACK(on_reset));
	gtk_style_context_add_class(gtk_widget_get_style_context(row), "destructive-action");
	gtk_widget_set_halign(row, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(p), row, FALSE, FALSE, 8);
	if (live_system()) {
		gtk_widget_set_sensitive(row, FALSE);
		gtk_box_pack_start(GTK_BOX(p), label(_("Not needed on the live system: it starts "
			"fresh every time."), "dim"), FALSE, FALSE, 0);
	}

	side = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
	gtk_widget_set_size_request(side, 150, -1);

	mg_status = label("", NULL);
	g_object_set(mg_status, "margin", 12, NULL);   /* a label has no border width */
	tz_status_label = mg_status;
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
		gtk_label_set_text(GTK_LABEL(auth_err), _("Wrong password."));
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

	auth_dlg = gtk_dialog_new_with_buttons(_("Users and system"), NULL, 0,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("OK"), GTK_RESPONSE_OK, NULL);
	gtk_window_set_icon_name(GTK_WINDOW(auth_dlg), "system-users");
	box = gtk_dialog_get_content_area(GTK_DIALOG(auth_dlg));
	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	gtk_box_pack_start(GTK_BOX(box), label(_("Changing users and system settings needs "
		"the administrator (root) password."), NULL), FALSE, FALSE, 0);
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

	fb_argv = argv;
	if (geteuid() == 0 && getenv("MY_SETUP_USER") == NULL)
		i18n_init_system();               /* the setup: the system's language */
	else
		i18n_init();                      /* the session's (my-admin passes it on) */
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
