/*
 * my-vpnctl: the part of the VPN window (my-vpn) that needs root rights.
 *
 *   my-vpnctl list             the saved VPNs, one name per line
 *   my-vpnctl import NAME      saves the WireGuard config on standard input
 *                              as /etc/my-distro/vpn/NAME.conf
 *   my-vpnctl remove NAME      deletes it
 *   my-vpnctl up NAME          connects (only one VPN at a time)
 *   my-vpnctl down             disconnects
 *   my-vpnctl status           "connected NAME" with details, or "disconnected"
 *
 * Installed setuid root. Like Wi-Fi, members of the group netdev may use it
 * (every user the setup or "Users and system" makes is one), and root.
 *
 * The config is read from standard input, so this program never opens a
 * file with root rights for the user. It only keeps the settings WireGuard
 * clients need, checks each value's characters, and drops PreUp/PostUp/...
 * (commands that wg-quick would run as root). Connecting is done by the
 * shell script /usr/lib/my-distro/vpn, started with a clean environment.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_VPN "/etc/my-distro/vpn"
#define SCRIPT  "/usr/lib/my-distro/vpn"
#define MAX_CONF (64 * 1024)

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

/* 1-15 letters, digits, - and _ (it is also shown as the VPN's name) */
static int valid_name(const char *s)
{
	size_t n = strlen(s);

	if (n < 1 || n > 15)
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_')
			return 0;
	return 1;
}

/* Keys, IP addresses, host names, ports, numbers: nothing else */
static int valid_value(const char *s)
{
	if (!*s)
		return 0;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("+/=.,:[]_-% ", *s))
			return 0;
	return 1;
}

static char *trim(char *s)
{
	char *e;

	while (isspace((unsigned char)*s))
		s++;
	e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

/* Reads a wg-quick style config from stdin, writes the cleaned version.
 * Messages for the user go to stdout. */
static int import(const char *name)
{
	static const char *iface_keys[] = { "PrivateKey", "Address", "DNS", "MTU", "ListenPort", NULL };
	static const char *peer_keys[] = { "PublicKey", "PresharedKey", "AllowedIPs", "Endpoint",
	                                   "PersistentKeepalive", NULL };
	static const char *dropped[] = { "PreUp", "PostUp", "PreDown", "PostDown", "Table",
	                                 "SaveConfig", "FwMark", NULL };
	char *buf = calloc(1, MAX_CONF + 1), line[2048], path[256], tmp[260];
	size_t len = 0, r;
	int section = 0, peers = 0, have_key = 0, have_addr = 0, fd;
	FILE *in, *out;

	while (buf && len < MAX_CONF && (r = fread(buf + len, 1, MAX_CONF - len, stdin)) > 0)
		len += r;
	if (!buf || len == 0 || len >= MAX_CONF) {
		printf("This is not a WireGuard config file (empty or too big).\n");
		return 1;
	}
	if (mkdir(DIR_VPN, 0700) < 0 && errno != EEXIST)
		return 1;
	snprintf(path, sizeof path, DIR_VPN "/%s.conf", name);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0 || !(out = fdopen(fd, "w"))) {
		printf("Could not save the VPN.\n");
		return 1;
	}
	in = fmemopen(buf, len, "r");
	fprintf(out, "# VPN \"%s\", imported with the VPN window (my-vpn)\n", name);
	while (in && fgets(line, sizeof line, in)) {
		char *s, *eq, *key, *val;
		int ok = 0;

		line[strcspn(line, "\r\n")] = '\0';
		if ((s = strchr(line, '#')))
			*s = '\0';
		s = trim(line);
		if (!*s)
			continue;
		if (*s == '[') {
			if (strcasecmp(s, "[Interface]") == 0)
				section = 1;
			else if (strcasecmp(s, "[Peer]") == 0)
				section = 2, peers++;
			else {
				printf("Unknown section %.40s.\n", s);
				goto bad;
			}
			fprintf(out, "\n%s\n", section == 1 ? "[Interface]" : "[Peer]");
			continue;
		}
		if (!(eq = strchr(s, '='))) {
			printf("Unexpected line: %.60s\n", s);
			goto bad;
		}
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);
		for (int i = 0; dropped[i]; i++)
			if (strcasecmp(key, dropped[i]) == 0) {
				printf("Left out: %s (my-distro doesn't run commands from VPN files).\n",
				       dropped[i]);
				ok = 2;
			}
		if (ok == 2)
			continue;
		for (int i = 0; section == 1 && iface_keys[i]; i++)
			if (strcasecmp(key, iface_keys[i]) == 0)
				key = (char *)iface_keys[i], ok = 1;
		for (int i = 0; section == 2 && peer_keys[i]; i++)
			if (strcasecmp(key, peer_keys[i]) == 0)
				key = (char *)peer_keys[i], ok = 1;
		if (!ok) {
			printf("Unknown setting %.30s.\n", key);
			goto bad;
		}
		if (!valid_value(val)) {
			printf("The value of %s has characters that don't belong there.\n", key);
			goto bad;
		}
		have_key |= strcmp(key, "PrivateKey") == 0;
		have_addr |= strcmp(key, "Address") == 0;
		fprintf(out, "%s = %s\n", key, val);
	}
	if (!have_key || !have_addr || peers == 0) {
		printf("This is not a WireGuard client config: it needs PrivateKey and Address "
		       "under [Interface], and a [Peer].\n");
		goto bad;
	}
	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		printf("Could not save the VPN.\n");
		return 1;
	}
	fclose(in);
	memset(buf, 0, len);              /* the private key */
	free(buf);
	return 0;
bad:
	fclose(out);
	unlink(tmp);
	if (in)
		fclose(in);
	memset(buf, 0, len);
	free(buf);
	return 1;
}

static int list(void)
{
	DIR *d = opendir(DIR_VPN);
	struct dirent *e;

	while (d && (e = readdir(d))) {
		size_t n = strlen(e->d_name);

		if (n > 5 && strcmp(e->d_name + n - 5, ".conf") == 0) {
			e->d_name[n - 5] = '\0';
			if (valid_name(e->d_name))
				printf("%s\n", e->d_name);
		}
	}
	if (d)
		closedir(d);
	return 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: my-vpnctl list | status | import NAME | remove NAME | up NAME | down\n");
	return 1;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "", *name = argc > 2 ? argv[2] : "";
	int needs_name = !strcmp(cmd, "import") || !strcmp(cmd, "remove") || !strcmp(cmd, "up");
	char path[256];

	if (!allowed()) {
		printf("Your user may not change the VPN (it is not in the group netdev).\n");
		return 1;
	}
	if (needs_name && !valid_name(name))
		return usage();
	if (setgid(0) != 0 || setuid(0) != 0)
		return 1;
	umask(077);
	clearenv();
	setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);

	if (!strcmp(cmd, "list"))
		return list();
	if (!strcmp(cmd, "import"))
		return import(name);
	if (!strcmp(cmd, "remove")) {
		snprintf(path, sizeof path, DIR_VPN "/%s.conf", name);
		return unlink(path) == 0 ? 0 : 1;
	}
	if (!strcmp(cmd, "up") || !strcmp(cmd, "down") || !strcmp(cmd, "status")) {
		fflush(stdout);
		execl(SCRIPT, "vpn", cmd, needs_name ? name : NULL, (char *)NULL);
		return 1;
	}
	return usage();
}
