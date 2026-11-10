/*
 * my-admin: opens "Users and system" (my-setup --manage) as root, after
 * checking the root password. The Disks window, the Backup window and the
 * Software app use it the same way.
 *
 * Installed setuid root (like my-power), so a normal user can start it.
 * The window, started as that user, shows the password field and writes the
 * password into our standard input. We compare it with /etc/shadow the same
 * way the login screen does (crypt() with the stored salt).
 *
 *   my-admin                 my-setup --manage        (Users and system)
 *   my-admin --update        my-setup --manage --update (its System update page)
 *   my-admin --disks         my-disks --manage        (the Disks window)
 *   my-admin --backup ARGS   my-backup ARGS           (backups with the system
 *                                                     settings, see my-backup)
 *   my-admin --pkg install|remove|upgrade [--with-dependents] NAME...
 *                            my-pkg with that command (the Software app)
 *   my-admin --sharing ARGS  /usr/lib/my-distro/sharing ARGS (Sharing window)
 *   my-admin --firmware ARGS /usr/lib/my-distro/firmware ARGS (Firmware
 *                            updates in System update)
 *   my-admin --secureboot ARGS  my-secureboot ARGS (Settings, Secure Boot)
 *                            (0.9.7; the arguments are checked like --backup's,
 *                            letters beyond ASCII allowed: folder names)
 *
 * It can only start these programs, with a clean environment (only the
 * session's language and proxy are passed on, checked). For --backup
 * the arguments are passed on when they are plain words and paths (no
 * options of other programs can sneak in that way). Package names are
 * checked: lower-case letters, digits and - _ . +, as in the package list.
 *
 * Exit codes: 2 = wrong password, 1 = other error; otherwise the program's.
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <ctype.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MY_PKG  "/usr/bin/my-pkg"
#define MAX_PKG 64                       /* names in one --pkg command */

static int root_password_ok(const char *typed)
{
	struct spwd *sp = getspnam("root");
	const char *hash = sp ? sp->sp_pwdp : NULL;
	char *h;

	if (!hash || hash[0] == '!' || hash[0] == '*')
		return 0;                       /* root account locked */
	if (hash[0] == '\0')
		return 1;                       /* no root password set */
	h = crypt(typed, hash);
	return h && strcmp(h, hash) == 0;
}

/* DISPLAY like ":0" or ":0.0"; anything else is not passed on. */
static int valid_display(const char *d)
{
	if (!d || d[0] != ':' || !isdigit((unsigned char)d[1]))
		return 0;
	for (d++; *d; d++)
		if (!isdigit((unsigned char)*d) && *d != '.')
			return 0;
	return 1;
}

/* An argument for my-backup: a word (backup, restore, --settings, a user
 * name, a snapshot's name) or an absolute path, without "..". Spaces are
 * fine (a USB stick called "MY STICK"). */
static int valid_arg(const char *s)
{
	if (!*s || strlen(s) > 400 || strstr(s, ".."))
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("/-_.: +", *s))
			return 0;
	return 1;
}

/* The root helpers of 0.9.7's windows, and their arguments: like
 * valid_arg, but also letters beyond ASCII (UTF-8, a folder "Öffentlich"),
 * and never starting with "-" (no options) */
static const struct { const char *mode, *prog; } helpers[] = {
	{ "--sharing", "/usr/lib/my-distro/sharing" },
	{ "--firmware", "/usr/lib/my-distro/firmware" },
	{ "--secureboot", "/usr/sbin/my-secureboot" },
};

static int valid_helper_arg(const char *s)
{
	if (!*s || *s == '-' || strlen(s) > 400 || strstr(s, ".."))
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("/-_.: +", *s) && (unsigned char)*s < 0x80)
			return 0;
	return 1;
}

/* Values passed on from the user's session: the language (Settings app) and
 * the proxy (for my-update). Only plain printable text, no spaces. The
 * language names a locale and a translation: letters, digits and _.@:-
 * only, never a path (a translation file from elsewhere could trick a
 * program that runs as root). */
static const char *pass_on[] = { "LANG", "LANGUAGE", "http_proxy", "https_proxy",
                                 "no_proxy", NULL };
static char kept[5][200];

static int plain(const char *v, int is_language)
{
	if (!v || !*v || strlen(v) >= sizeof kept[0])
		return 0;
	if (is_language)
		return strspn(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
		                 "0123456789_.@:-") == strlen(v);
	for (; *v; v++)
		if (*v <= ' ' || *v == 0x7f)
			return 0;
	return 1;
}

/* A package name from the list: a-z, 0-9, - _ . +, starting with a letter or digit */
static int valid_package(const char *s)
{
	size_t n = strlen(s);

	if (n == 0 || n > 64 || !(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0])))
		return 0;
	for (; *s; s++)
		if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && !strchr("-_.+", *s))
			return 0;
	return 1;
}

/* --pkg CMD [--with-dependents] NAME...: checks it all, fills args for my-pkg */
static int pkg_args(int argc, char **argv, char **args)
{
	int n = 0;

	if (argc < 3 || argc - 2 > MAX_PKG)
		return 0;
	if (strcmp(argv[2], "install") && strcmp(argv[2], "remove") && strcmp(argv[2], "upgrade"))
		return 0;
	args[n++] = "my-pkg";
	args[n++] = argv[2];
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--with-dependents") == 0 && strcmp(argv[2], "remove") == 0)
			args[n++] = argv[i];
		else if (valid_package(argv[i]))
			args[n++] = argv[i];
		else
			return 0;
	}
	args[n] = NULL;
	return 1;
}

int main(int argc, char **argv)
{
	char pass[256] = "", display[16] = ":0", user[64] = "";
	struct passwd *me = getpwuid(getuid());
	const char *mode = argc > 1 ? argv[1] : "";
	char *pkg_argv[MAX_PKG + 4];

	if (strcmp(mode, "--backup") == 0)
		for (int i = 2; i < argc; i++)
			if (!valid_arg(argv[i])) {
				fprintf(stderr, "my-admin: odd argument\n");
				return 1;
			}
	if (strcmp(mode, "--pkg") == 0 && !pkg_args(argc, argv, pkg_argv)) {
		fprintf(stderr, "usage: my-admin --pkg install|remove|upgrade [--with-dependents] NAME...\n");
		return 1;
	}
	for (size_t h = 0; h < sizeof helpers / sizeof *helpers; h++)
		if (strcmp(mode, helpers[h].mode) == 0)
			for (int i = 2; i < argc; i++)
				if (!valid_helper_arg(argv[i])) {
					fprintf(stderr, "my-admin: odd argument\n");
					return 1;
				}

	/* Only the first line is ours: unbuffered, so the rest of the input
	 * (the sharing password for --sharing) is left for the program */
	setvbuf(stdin, NULL, _IONBF, 0);
	if (!fgets(pass, sizeof pass, stdin))
		pass[0] = '\0';
	pass[strcspn(pass, "\n")] = '\0';

	if (getuid() != 0 && !root_password_ok(pass)) {
		memset(pass, 0, sizeof pass);
		sleep(1);                        /* slows down guessing */
		return 2;
	}
	memset(pass, 0, sizeof pass);

	if (valid_display(getenv("DISPLAY")))
		snprintf(display, sizeof display, "%s", getenv("DISPLAY"));
	if (me)
		snprintf(user, sizeof user, "%s", me->pw_name);
	for (int i = 0; pass_on[i]; i++)
		if (plain(getenv(pass_on[i]), i < 2))
			snprintf(kept[i], sizeof kept[i], "%s", getenv(pass_on[i]));

	if (setgid(0) != 0 || setuid(0) != 0)
		return 1;
	clearenv();
	setenv("HOME", "/root", 1);
	setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
	setenv("DISPLAY", display, 1);
	setenv("LANG", "en_US.UTF-8", 1);
	for (int i = 0; pass_on[i]; i++)
		if (kept[i][0])
			setenv(pass_on[i], kept[i], 1);
	setenv("NO_AT_BRIDGE", "1", 1);
	setenv("MY_SETUP_USER", user, 1);   /* who asked (my-setup: can't remove themselves) */

	if (strcmp(mode, "--update") == 0)
		execl("/usr/bin/my-setup", "my-setup", "--manage", "--update", (char *)NULL);
	else if (strcmp(mode, "--disks") == 0)
		execl("/usr/bin/my-disks", "my-disks", "--manage", (char *)NULL);
	else if (strcmp(mode, "--backup") == 0) {
		argv[1] = "my-backup";
		execv("/usr/bin/my-backup", argv + 1);
	} else if (strcmp(mode, "--pkg") == 0) {
		execv(MY_PKG, pkg_argv);
	} else {
		for (size_t h = 0; h < sizeof helpers / sizeof *helpers; h++)
			if (strcmp(mode, helpers[h].mode) == 0) {
				argv[1] = (char *)helpers[h].prog;
				execv(helpers[h].prog, argv + 1);
				return 1;
			}
	}
	if (strcmp(mode, "--update") && strcmp(mode, "--disks") && strcmp(mode, "--backup") &&
	    strcmp(mode, "--pkg"))
		execl("/usr/bin/my-setup", "my-setup", "--manage", (char *)NULL);
	return 1;
}
