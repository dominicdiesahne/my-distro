/*
 * my-wificert: the part of the Wi-Fi window (my-wifi) that needs root rights
 * for networks that log in with certificates (EAP-TLS): it keeps their
 * files where only root can read them.
 *
 *   my-wificert save SSIDHEX KIND   saves standard input (a certificate or a
 *                                   private key) as
 *                                   /etc/my-distro/wifi-certs/SSIDHEX-KIND
 *                                   and prints that path
 *   my-wificert remove SSIDHEX      deletes the files of that network
 *
 * SSIDHEX is the network's name as hex, KIND is ca, cert or key. Installed
 * setuid root. Like Wi-Fi and my-vpnctl, members of the group netdev may use
 * it, and root. The file comes in on standard input, so this program never
 * opens a file with root rights for the user. wpa_supplicant (root) reads
 * the files from there.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_CERTS "/etc/my-distro/wifi-certs"
#define MAX_FILE (128 * 1024)

static const char *const kinds[] = { "ca", "cert", "key", NULL };

/* root, or the real user is in the group netdev */
static int allowed(void)
{
	struct group *g;
	gid_t groups[256];
	int n;

	if (getuid() == 0)
		return 1;
	g = getgrnam("netdev");
	if (!g)
		return 0;
	if (getgid() == g->gr_gid)
		return 1;
	n = getgroups(256, groups);
	for (int i = 0; i < n; i++)
		if (groups[i] == g->gr_gid)
			return 1;
	return 0;
}

/* 2 to 64 hex digits (a Wi-Fi name has up to 32 bytes) */
static int valid_hex(const char *s)
{
	size_t n = strlen(s);

	if (n < 2 || n > 64 || n % 2)
		return 0;
	for (; *s; s++)
		if (!isxdigit((unsigned char)*s))
			return 0;
	return 1;
}

static int save(const char *hex, const char *kind)
{
	char *buf = malloc(MAX_FILE + 1), path[256], tmp[270];
	size_t len = 0, r;
	int fd, ok = 0;

	for (int i = 0; ; i++) {
		if (!kinds[i])
			return 1;
		if (strcmp(kind, kinds[i]) == 0)
			break;
	}
	if (!buf)
		return 1;
	while (len < MAX_FILE && (r = fread(buf + len, 1, MAX_FILE - len, stdin)) > 0)
		len += r;
	if (len == 0 || len >= MAX_FILE) {
		printf("The file is empty or too big.\n");
		free(buf);
		return 1;
	}
	if (mkdir(DIR_CERTS, 0700) < 0 && errno != EEXIST) {
		free(buf);
		return 1;
	}
	snprintf(path, sizeof path, DIR_CERTS "/%s-%s", hex, kind);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd >= 0) {
		ok = write(fd, buf, len) == (ssize_t)len;
		ok &= close(fd) == 0;
	}
	memset(buf, 0, len);              /* it may be the private key */
	free(buf);
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return 1;
	}
	printf("%s\n", path);
	return 0;
}

static int remove_all(const char *hex)
{
	char path[256];

	for (int i = 0; kinds[i]; i++) {
		snprintf(path, sizeof path, DIR_CERTS "/%s-%s", hex, kinds[i]);
		unlink(path);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "", *hex = argc > 2 ? argv[2] : "";

	if (!allowed()) {
		printf("Your user may not change Wi-Fi (it is not in the group netdev).\n");
		return 1;
	}
	if (!valid_hex(hex) || (!strcmp(cmd, "save") && argc < 4) ||
	    (strcmp(cmd, "save") && strcmp(cmd, "remove"))) {
		fprintf(stderr, "usage: my-wificert save SSIDHEX ca|cert|key | remove SSIDHEX\n");
		return 1;
	}
	if (setgid(0) != 0 || setuid(0) != 0)
		return 1;
	umask(077);
	if (!strcmp(cmd, "save"))
		return save(hex, argv[3]);
	return remove_all(hex);
}
