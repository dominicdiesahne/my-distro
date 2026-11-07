/*
 * my-checkpass: is this the password of the user who runs me? For the
 * screen lock (my-lock), which runs as you and can't read /etc/shadow.
 *
 *     echo PASSWORD | my-checkpass     exit 0 = right, 2 = wrong
 *
 * Installed setuid root (like my-power) only to read /etc/shadow. It checks
 * the password of the user who started it (getuid), never anyone else's, and
 * waits two seconds after a wrong one, so guessing takes forever.
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	char pass[256] = "";
	struct passwd *pw = getpwuid(getuid());
	struct spwd *sp;
	const char *hash;
	char *typed;
	int ok = 0;

	if (!fgets(pass, sizeof pass, stdin))
		pass[0] = '\0';
	pass[strcspn(pass, "\n")] = '\0';

	if (pw) {
		hash = pw->pw_passwd;
		if ((sp = getspnam(pw->pw_name)))
			hash = sp->sp_pwdp;
		if (hash && hash[0] != '!' && hash[0] != '*') {
			if (hash[0] == '\0')
				ok = 1;                  /* no password set */
			else if ((typed = crypt(pass, hash)))
				ok = strcmp(typed, hash) == 0;
		}
	}
	memset(pass, 0, sizeof pass);
	if (!ok) {
		sleep(2);                        /* slows down guessing */
		return 2;
	}
	return 0;
}
