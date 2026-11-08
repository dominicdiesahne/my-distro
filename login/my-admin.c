/*
 * my-admin: opens "Users and system" (my-setup --manage) as root, after
 * checking the root password.
 *
 * Installed setuid root (like my-power), so a normal user can start it.
 * my-setup, started as that user, shows the password field and writes the
 * password into our standard input. We compare it with /etc/shadow the same
 * way the login screen does (crypt() with the stored salt).
 *
 * It can only start that one program, with a clean environment, so the root
 * rights can't be used for anything else. The only argument it passes on is
 * "--update" (open the window on its System update page).
 *
 * The Software app (my-software) uses it the same way to install and remove
 * programs: "my-admin --pkg install|remove|upgrade [--with-dependents] NAME..."
 * runs /usr/bin/my-pkg with that command (and nothing else). Names are
 * checked: lower-case letters, digits and - _ . +, as in the package list.
 *
 * Exit codes: 2 = wrong password, 1 = other error; otherwise my-setup's
 * (or my-pkg's).
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

#define PROGRAM "/usr/bin/my-setup"
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
	int update = argc > 1 && strcmp(argv[1], "--update") == 0;
	int pkg = argc > 1 && strcmp(argv[1], "--pkg") == 0;
	char *pkg_argv[MAX_PKG + 4];

	if (pkg && !pkg_args(argc, argv, pkg_argv)) {
		fprintf(stderr, "usage: my-admin --pkg install|remove|upgrade [--with-dependents] NAME...\n");
		return 1;
	}

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

	if (setgid(0) != 0 || setuid(0) != 0)
		return 1;
	clearenv();
	setenv("HOME", "/root", 1);
	setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
	setenv("DISPLAY", display, 1);
	setenv("LANG", "en_US.UTF-8", 1);
	setenv("NO_AT_BRIDGE", "1", 1);
	setenv("MY_SETUP_USER", user, 1);   /* who asked: can't remove themselves */
	if (pkg) {
		execv(MY_PKG, pkg_argv);
		return 1;
	}
	if (update)
		execl(PROGRAM, "my-setup", "--manage", "--update", (char *)NULL);
	else
		execl(PROGRAM, "my-setup", "--manage", (char *)NULL);
	return 1;
}
