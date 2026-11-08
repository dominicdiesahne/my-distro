/*
 * my-admin: opens "Users and system" (my-setup --manage) as root, after
 * checking the root password. The Disks window and the Backup window use it
 * the same way.
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
 *
 * It can only start these programs, with a clean environment. For --backup
 * the arguments are passed on when they are plain words and paths (no
 * options of other programs can sneak in that way).
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

int main(int argc, char **argv)
{
	char pass[256] = "", display[16] = ":0", user[64] = "";
	struct passwd *me = getpwuid(getuid());
	const char *mode = argc > 1 ? argv[1] : "";

	if (strcmp(mode, "--backup") == 0)
		for (int i = 2; i < argc; i++)
			if (!valid_arg(argv[i])) {
				fprintf(stderr, "my-admin: odd argument\n");
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
	setenv("MY_SETUP_USER", user, 1);   /* who asked (my-setup: can't remove themselves) */

	if (strcmp(mode, "--update") == 0)
		execl("/usr/bin/my-setup", "my-setup", "--manage", "--update", (char *)NULL);
	else if (strcmp(mode, "--disks") == 0)
		execl("/usr/bin/my-disks", "my-disks", "--manage", (char *)NULL);
	else if (strcmp(mode, "--backup") == 0) {
		argv[1] = "my-backup";
		execv("/usr/bin/my-backup", argv + 1);
	} else
		execl("/usr/bin/my-setup", "my-setup", "--manage", (char *)NULL);
	return 1;
}
