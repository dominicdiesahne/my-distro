/*
 * my-vpnctl: the part of the VPN window (my-vpn) that needs root rights.
 *
 *   my-vpnctl list             the saved VPNs, one per line: NAME, a tab and
 *                              "wg" (WireGuard) or "ovpn" (OpenVPN)
 *   my-vpnctl import NAME      saves the WireGuard config on standard input
 *                              as /etc/my-distro/vpn/NAME.conf
 *   my-vpnctl import-ovpn NAME the same for an OpenVPN config (NAME.ovpn)
 *   my-vpnctl auth NAME        the user name and password (two lines on
 *                              standard input) for an OpenVPN config that
 *                              has auth-user-pass (NAME.auth, root only)
 *   my-vpnctl keypass NAME     the password of the private key in an
 *                              OpenVPN config (one line on standard input;
 *                              NAME.pass, root only, given to openvpn with
 *                              --askpass)
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
 * and OpenVPN clients need, checks each value's characters, and drops
 * PreUp/PostUp/... and OpenVPN's up/down/plugin/... (commands that would
 * run as root). Connecting is done by the shell script
 * /usr/lib/my-distro/vpn, started with a clean environment.
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
#define MAX_CONF (128 * 1024)   /* the VPN window puts ca, cert, key... in */

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
		if (!isalnum((unsigned char)*s) && !strchr("+/=.,:[]_-% @", *s))
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

/* --- OpenVPN ---------------------------------------------------------------- */

/* The client settings an .ovpn file may have. openvpn runs as root, and some
 * settings make it run programs, write files or read files: those are not
 * here. Settings that only point at a file (ca, cert, key, ...) are accepted
 * as blocks inside the file (<ca> ... </ca>), not as file names. */
static const char *const ovpn_keys[] = {
	"client", "dev", "dev-type", "proto", "remote", "remote-random", "remote-random-hostname",
	"remote-cert-tls", "remote-cert-ku", "remote-cert-eku", "nobind", "persist-key",
	"persist-tun", "persist-local-ip", "persist-remote-ip", "resolv-retry", "cipher",
	"data-ciphers", "data-ciphers-fallback", "ncp-ciphers", "auth", "tls-version-min",
	"tls-version-max", "tls-cipher", "tls-ciphersuites", "tls-client", "comp-lzo",
	"compress", "allow-compression", "key-direction", "auth-nocache", "auth-retry",
	"route", "route-ipv6", "route-nopull", "route-delay", "route-metric", "pull",
	"pull-filter", "ping", "ping-restart", "keepalive", "reneg-sec", "reneg-bytes",
	"mssfix", "tun-mtu", "link-mtu", "fragment", "sndbuf", "rcvbuf", "explicit-exit-notify",
	"verify-x509-name", "ns-cert-type", "replay-window", "ignore-unknown-option", "float",
	"lport", "rport", "local", "connect-retry", "connect-retry-max", "connect-timeout",
	"server-poll-timeout", "tls-timeout", "hand-window", "tran-window", "dhcp-option",
	"topology", "fast-io", "passtos", "http-proxy", "http-proxy-retry", "http-proxy-timeout",
	"socks-proxy", "socks-proxy-retry", "mute-replay-warnings", "key-method", "peer-fingerprint",
	"push-peer-info", "tls-exit", "redirect-gateway", "auth-user-pass", "bind", NULL };
/* settings that run commands, read or write files, or that my-distro sets itself */
static const char *const ovpn_dropped[] = {
	"up", "down", "route-up", "route-pre-down", "ipchange", "tls-verify", "client-connect",
	"client-disconnect", "learn-address", "auth-user-pass-verify", "plugin", "script-security",
	"setenv", "setenv-safe", "up-restart", "down-pre", "management", "config", "cd", "chroot",
	"log", "log-append", "status", "writepid", "daemon", "user", "group", "dev-node",
	"tmp-dir", "ifconfig-noexec", "echo", "syslog", "askpass", "verb", "mute", "block-outside-dns",
	"register-dns", "winnt-fix", "ip-win32", "route-method", "service", "show-net-up", "engine",
	"auth-gen-token", "management-client-user", NULL };
/* settings that name a file: accepted only as a <block> */
static const char *const ovpn_files[] = { "ca", "cert", "key", "tls-auth", "tls-crypt",
	"tls-crypt-v2", "extra-certs", "dh", "secret", "pkcs12", "crl-verify", "capath", NULL };

static int in_list(const char *const *list, const char *s)
{
	for (int i = 0; list[i]; i++)
		if (strcasecmp(s, list[i]) == 0)
			return 1;
	return 0;
}

/* One line inside a <block>: PEM or key text, nothing else */
static int valid_block_line(const char *s)
{
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("+/=-: ,.", *s))
			return 0;
	return 1;
}

/* Reads an .ovpn file from stdin, writes the cleaned version. Messages for
 * the user go to stdout. */
static int import_ovpn(const char *name)
{
	char *buf = calloc(1, MAX_CONF + 1), line[2048], path[256], tmp[260], block[32] = "";
	size_t len = 0, r;
	int fd, remotes = 0;
	FILE *in = NULL, *out;

	while (buf && len < MAX_CONF && (r = fread(buf + len, 1, MAX_CONF - len, stdin)) > 0)
		len += r;
	if (!buf || len == 0 || len >= MAX_CONF) {
		printf("This is not an OpenVPN config file (empty or too big).\n");
		return 1;
	}
	if (mkdir(DIR_VPN, 0700) < 0 && errno != EEXIST)
		return 1;
	snprintf(path, sizeof path, DIR_VPN "/%s.ovpn", name);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0 || !(out = fdopen(fd, "w"))) {
		printf("Could not save the VPN.\n");
		return 1;
	}
	in = fmemopen(buf, len, "r");
	fprintf(out, "# VPN \"%s\", imported with the VPN window (my-vpn)\n", name);
	while (in && fgets(line, sizeof line, in)) {
		char *s, *sp, *key, *val;

		line[strcspn(line, "\r\n")] = '\0';
		s = trim(line);
		if (block[0]) {                    /* inside <ca> ... </ca> */
			if (s[0] == '<' && s[1] == '/') {
				if (strncasecmp(s + 2, block, strlen(block)) != 0 ||
				    strcmp(s + 2 + strlen(block), ">") != 0) {
					printf("A <%s> block is not closed.\n", block);
					goto bad;
				}
				fprintf(out, "</%s>\n", block);
				block[0] = '\0';
			} else if (!*s || *s == '#' || *s == ';') {
				continue;
			} else if (valid_block_line(s)) {
				fprintf(out, "%s\n", s);
			} else {
				printf("The <%s> block has characters that don't belong there.\n", block);
				goto bad;
			}
			continue;
		}
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '<') {
			char *e = strchr(s, '>');

			if (!e || e[1] || e - s - 1 >= (int)sizeof block) {
				printf("Unexpected line: %.60s\n", s);
				goto bad;
			}
			*e = '\0';
			if (!in_list(ovpn_files, s + 1)) {
				printf("Unknown block %.30s>.\n", s);
				goto bad;
			}
			snprintf(block, sizeof block, "%s", s + 1);
			fprintf(out, "<%s>\n", block);
			continue;
		}
		key = s;
		if ((sp = strpbrk(s, " \t"))) {
			*sp = '\0';
			val = trim(sp + 1);
		} else {
			val = s + strlen(s);
		}
		if (in_list(ovpn_dropped, key)) {
			printf("Left out: %s (my-distro doesn't run commands from VPN files).\n", key);
			continue;
		}
		if (in_list(ovpn_files, key)) {
			/* the VPN window reads such files itself and puts them in as blocks */
			printf("%s points to a file. Import the config with the VPN window (it "
			       "takes the file from the same folder), or put the file's content "
			       "into the config (between <%s> and </%s>).\n", key, key, key);
			goto bad;
		}
		if (!in_list(ovpn_keys, key)) {
			printf("Unknown setting %.30s.\n", key);
			goto bad;
		}
		if (*val && !valid_value(val)) {
			printf("The value of %s has characters that don't belong there.\n", key);
			goto bad;
		}
		if (strcasecmp(key, "auth-user-pass") == 0 && *val) {
			printf("auth-user-pass names a file: leave the name out, the window "
			       "asks for the user name and password.\n");
			goto bad;
		}
		if ((strcasecmp(key, "dev") == 0 || strcasecmp(key, "dev-type") == 0) &&
		    strncmp(val, "tun", 3) != 0) {
			printf("Only tunnels (dev tun) work, not bridges (tap).\n");
			goto bad;
		}
		if (strcasecmp(key, "http-proxy") == 0 && strchr(val, ' ') &&
		    strchr(strchr(val, ' ') + 1, ' ')) {
			printf("http-proxy with a login file is not supported.\n");
			goto bad;
		}
		remotes += strcasecmp(key, "remote") == 0;
		if (strcasecmp(key, "redirect-gateway") == 0 && !strstr(val, "def1"))
			fprintf(out, "redirect-gateway def1%s%s\n", *val ? " " : "", val);  /* keeps the old default route */
		else if (*val)
			fprintf(out, "%s %s\n", key, val);
		else
			fprintf(out, "%s\n", key);
	}
	if (block[0]) {
		printf("A <%s> block is not closed.\n", block);
		goto bad;
	}
	if (remotes == 0) {
		printf("This is not an OpenVPN client config: it has no remote (server).\n");
		goto bad;
	}
	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		printf("Could not save the VPN.\n");
		return 1;
	}
	fclose(in);
	memset(buf, 0, len);              /* it may hold the private key */
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

/* auth NAME: the user name and the password (two lines) on standard input,
 * for an OpenVPN config that has auth-user-pass */
static int save_auth(const char *name)
{
	char user[256], pass[256], path[256], tmp[260];
	int fd;
	FILE *out;

	snprintf(path, sizeof path, DIR_VPN "/%s.ovpn", name);
	if (access(path, F_OK) != 0)
		return 1;
	if (!fgets(user, sizeof user, stdin) || !fgets(pass, sizeof pass, stdin))
		return 1;
	user[strcspn(user, "\r\n")] = '\0';
	pass[strcspn(pass, "\r\n")] = '\0';
	if (!user[0] || !pass[0])
		return 1;
	snprintf(path, sizeof path, DIR_VPN "/%s.auth", name);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0 || !(out = fdopen(fd, "w")))
		return 1;
	fprintf(out, "%s\n%s\n", user, pass);
	memset(pass, 0, sizeof pass);
	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return 1;
	}
	return 0;
}

/* keypass NAME: the password of the OpenVPN config's private key (one line
 * on standard input) */
static int save_keypass(const char *name)
{
	char pass[1024], path[256], tmp[260];
	int fd;
	FILE *out;

	snprintf(path, sizeof path, DIR_VPN "/%s.ovpn", name);
	if (access(path, F_OK) != 0)
		return 1;
	if (!fgets(pass, sizeof pass, stdin))
		return 1;
	pass[strcspn(pass, "\r\n")] = '\0';
	if (!pass[0])
		return 1;
	snprintf(path, sizeof path, DIR_VPN "/%s.pass", name);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0 || !(out = fdopen(fd, "w"))) {
		memset(pass, 0, sizeof pass);
		return 1;
	}
	fprintf(out, "%s\n", pass);
	memset(pass, 0, sizeof pass);
	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return 1;
	}
	return 0;
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
				printf("%s\twg\n", e->d_name);
		} else if (n > 5 && strcmp(e->d_name + n - 5, ".ovpn") == 0) {
			e->d_name[n - 5] = '\0';
			if (valid_name(e->d_name))
				printf("%s\tovpn\n", e->d_name);
		}
	}
	if (d)
		closedir(d);
	return 0;
}

static int usage(void)
{
	fprintf(stderr, "usage: my-vpnctl list | status | import NAME | import-ovpn NAME | auth NAME | keypass NAME | remove NAME | up NAME | down\n");
	return 1;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "", *name = argc > 2 ? argv[2] : "";
	int needs_name = !strcmp(cmd, "import") || !strcmp(cmd, "import-ovpn") ||
	                !strcmp(cmd, "auth") || !strcmp(cmd, "keypass") || !strcmp(cmd, "remove") ||
	                !strcmp(cmd, "up");
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
	if (!strcmp(cmd, "import") || !strcmp(cmd, "import-ovpn")) {
		int ovpn = cmd[6] != '\0';

		if ((ovpn ? import_ovpn(name) : import(name)) != 0)
			return 1;
		/* a name belongs to one kind of VPN: the other one's files go */
		snprintf(path, sizeof path, DIR_VPN "/%s.%s", name, ovpn ? "conf" : "ovpn");
		unlink(path);
		snprintf(path, sizeof path, DIR_VPN "/%s.auth", name);
		unlink(path);
		snprintf(path, sizeof path, DIR_VPN "/%s.pass", name);
		unlink(path);
		return 0;
	}
	if (!strcmp(cmd, "auth"))
		return save_auth(name);
	if (!strcmp(cmd, "keypass"))
		return save_keypass(name);
	if (!strcmp(cmd, "remove")) {
		int gone = 0;

		snprintf(path, sizeof path, DIR_VPN "/%s.conf", name);
		gone |= unlink(path) == 0;
		snprintf(path, sizeof path, DIR_VPN "/%s.ovpn", name);
		gone |= unlink(path) == 0;
		snprintf(path, sizeof path, DIR_VPN "/%s.auth", name);
		unlink(path);
		snprintf(path, sizeof path, DIR_VPN "/%s.pass", name);
		unlink(path);
		return gone ? 0 : 1;
	}
	if (!strcmp(cmd, "up") || !strcmp(cmd, "down") || !strcmp(cmd, "status")) {
		fflush(stdout);
		execl(SCRIPT, "vpn", cmd, needs_name ? name : NULL, (char *)NULL);
		return 1;
	}
	return usage();
}
