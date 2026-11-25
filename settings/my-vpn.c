/*
 * my-vpn: the VPN window of my-distro (GTK 3), in the Start menu.
 *
 * VPN services and routers give you a WireGuard config file (NAME.conf, the
 * same file the official WireGuard apps import) or an OpenVPN file
 * (NAME.ovpn). "Import…" saves it (an OpenVPN file that wants a user name
 * asks for it and the password). Files an OpenVPN config points to (ca
 * ca.crt, cert, key, tls-auth ta.key 1, ...) are read from its folder here,
 * as the user, and put into it as <ca> ... </ca> blocks; a private key with
 * a password asks for that password. Then Connect / Disconnect. Only one VPN is
 * connected at a time. After a change of network (another Wi-Fi, a cable) a
 * connected VPN connects again by itself (/usr/lib/my-distro/vpn).
 *
 * The work that needs root is done by my-vpnctl (setuid, for members of the
 * group netdev like the Wi-Fi window), which runs /usr/lib/my-distro/vpn.
 */
#include <ctype.h>
#include <gtk/gtk.h>

#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#define CTL "/usr/bin/my-vpnctl"

static GtkWidget *win, *status_label, *detail_label, *list, *btn_connect, *btn_disconnect,
                 *btn_remove;
static char connected[32];        /* the connected VPN's name, or "" */
static int connected_ovpn;        /* ... and it is an OpenVPN one */
static int busy;                  /* my-vpnctl up/down is running */
static gint64 connected_since;

/* Runs my-vpnctl ARGS; returns what it printed (g_free it). input: written
 * to its standard input, or NULL. *ok: whether it exited with 0. */
static char *ctl(const char *a1, const char *a2, const char *input, int *ok)
{
	const char *argv[] = { CTL, a1, a2, NULL };
	char *out = NULL;
	int st = 1;

	*ok = 0;
	if (input) {
		/* g_spawn_sync can't feed stdin: through a pipe by hand */
		GPid pid;
		int in, outfd;
		GString *s = g_string_new(NULL);
		char buf[1024];
		ssize_t n;

		if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD |
		                              G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &pid,
		                              &in, &outfd, NULL, NULL))
			return g_string_free(s, FALSE);
		for (size_t done = 0, len = strlen(input); done < len; ) {
			ssize_t w = write(in, input + done, len - done);

			if (w <= 0)
				break;            /* my-vpnctl then says the file is cut off */
			done += w;
		}
		close(in);
		while ((n = read(outfd, buf, sizeof buf)) > 0)
			g_string_append_len(s, buf, n);
		close(outfd);
		waitpid(pid, &st, 0);
		g_spawn_close_pid(pid);
		*ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
		return g_string_free(s, FALSE);
	}
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, &st, NULL))
		return NULL;
	*ok = g_spawn_check_wait_status(st, NULL);
	return out;
}

static const char *selected_name(void)
{
	GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));

	return r ? g_object_get_data(G_OBJECT(r), "name") : NULL;
}

static void update_buttons(void)
{
	const char *n = selected_name();

	gtk_widget_set_sensitive(btn_connect, !busy && n && strcmp(n, connected) != 0);
	gtk_widget_set_sensitive(btn_disconnect, !busy && connected[0]);
	gtk_widget_set_sensitive(btn_remove, !busy && n && strcmp(n, connected) != 0);
}

static void fill_list(const char *select)
{
	GList *old = gtk_container_get_children(GTK_CONTAINER(list));
	int ok;
	char *out = ctl("list", NULL, NULL, &ok), **names;

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
	names = g_strsplit(out ? out : "", "\n", -1);
	for (int i = 0; names[i]; i++) {
		GtkWidget *row, *box, *name, *info;
		char *tab = strchr(names[i], '\t'), *kind, *text;

		if (!names[i][0])
			continue;
		kind = tab ? tab + 1 : "wg";
		if (tab)
			*tab = '\0';
		row = gtk_list_box_row_new();
		box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		name = gtk_label_new(names[i]);
		text = g_strdup_printf("%s%s", strcmp(kind, "ovpn") == 0 ? "OpenVPN" : "WireGuard",
		                       strcmp(names[i], connected) == 0 ? _("  ·  connected") : "");
		info = gtk_label_new(text);
		g_free(text);
		gtk_label_set_xalign(GTK_LABEL(name), 0);
		gtk_style_context_add_class(gtk_widget_get_style_context(info), "dim-label");
		g_object_set(box, "margin", 8, NULL);
		gtk_box_pack_start(GTK_BOX(box), name, TRUE, TRUE, 0);
		gtk_box_pack_end(GTK_BOX(box), info, FALSE, FALSE, 0);
		gtk_container_add(GTK_CONTAINER(row), box);
		g_object_set_data_full(G_OBJECT(row), "name", g_strdup(names[i]), g_free);
		gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
		if (select && strcmp(select, names[i]) == 0)
			gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
	}
	g_strfreev(names);
	g_free(out);
	gtk_widget_show_all(list);
	update_buttons();
}

static void human_bytes(char *buf, size_t len, double b)
{
	if (b < 1024 * 1024)
		snprintf(buf, len, "%.0f KB", b / 1024);
	else if (b < 1024.0 * 1024 * 1024)
		snprintf(buf, len, "%.1f MB", b / (1024 * 1024));
	else
		snprintf(buf, len, "%.2f GB", b / (1024.0 * 1024 * 1024));
}

/* Reads "my-vpnctl status" into the two labels */
static gboolean refresh_status(gpointer d)
{
	int ok;
	char *out, **lines, old[32], rx[32] = "", tx[32] = "", *text = NULL, *detail = NULL;
	const char *addr = "", *hs = "never";
	int ovpn = 0;

	(void)d;
	if (busy)
		return G_SOURCE_CONTINUE;
	out = ctl("status", NULL, NULL, &ok);
	g_strlcpy(old, connected, sizeof old);
	connected[0] = '\0';
	lines = g_strsplit(out ? out : "", "\n", -1);
	for (int i = 0; lines[i]; i++) {
		if (g_str_has_prefix(lines[i], "connected "))
			g_strlcpy(connected, lines[i] + 10, sizeof connected);
		else if (g_str_has_prefix(lines[i], "kind "))
			ovpn = strcmp(lines[i] + 5, "ovpn") == 0;
		else if (g_str_has_prefix(lines[i], "address "))
			addr = lines[i] + 8;
		else if (g_str_has_prefix(lines[i], "handshake "))
			hs = lines[i] + 10;
		else if (g_str_has_prefix(lines[i], "received "))
			human_bytes(rx, sizeof rx, atof(lines[i] + 9));
		else if (g_str_has_prefix(lines[i], "sent "))
			human_bytes(tx, sizeof tx, atof(lines[i] + 5));
	}
	if (connected[0]) {
		int secs = strcmp(hs, "never") == 0 ? -1 : atoi(hs);

		if (strcmp(old, connected) != 0)
			connected_since = g_get_monotonic_time();
		connected_ovpn = ovpn;
		text = g_strdup_printf(_("Connected to %s"), connected);
		/* OpenVPN: the daemon reports "connected" when the tunnel works */
		if (ovpn)
			detail = g_strdup_printf(_("Address %s\nOpenVPN · received %s · sent %s"),
			                         addr, rx, tx);
		/* WireGuard talks to the server at least every 2 minutes while
		 * something is sent; no answer at all means a wrong config or a
		 * blocked network */
		if (ovpn)
			;
		else if (secs < 0 && g_get_monotonic_time() - connected_since > 15 * G_USEC_PER_SEC)
			detail = g_strdup_printf(_("Address %s\nNo answer from the VPN server yet. Is the "
			                         "config right, and does this network allow VPNs?"), addr);
		else if (secs < 0)
			detail = g_strdup_printf(_("Address %s\nWaiting for the VPN server…"), addr);
		else
			detail = g_strdup_printf(_("Address %s\nLast answer from the server %d s ago · "
			                         "received %s · sent %s"), addr, secs, rx, tx);
	} else {
		text = g_strdup(_("Not connected."));
		detail = g_strdup(_("Import the config file from your VPN service or router "
		                  "(WireGuard .conf or OpenVPN .ovpn), then connect."));
	}
	gtk_label_set_text(GTK_LABEL(status_label), text);
	gtk_label_set_text(GTK_LABEL(detail_label), detail);
	if (strcmp(old, connected) != 0)
		fill_list(selected_name());
	g_free(text);
	g_free(detail);
	g_strfreev(lines);
	g_free(out);
	return G_SOURCE_CONTINUE;
}

static void message(GtkMessageType type, const char *text, const char *detail)
{
	GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, type,
	                                      GTK_BUTTONS_CLOSE, "%s", text);

	if (detail && detail[0])
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", detail);
	gtk_dialog_run(GTK_DIALOG(m));
	gtk_widget_destroy(m);
}

/* up/down take a few seconds (the server's name is looked up): in the
 * background, so the window stays responsive */
static void on_done(GPid pid, gint st, gpointer action)
{
	g_spawn_close_pid(pid);
	busy = 0;
	refresh_status(NULL);
	if (!g_spawn_check_wait_status(st, NULL) && strcmp(action, "up") == 0) {
		gtk_label_set_text(GTK_LABEL(status_label), _("Could not connect."));
		gtk_label_set_text(GTK_LABEL(detail_label), _("Check the config file, the user name "
			"and password (import the file again to change them) and that this network "
			"allows VPNs."));
	}
	update_buttons();
}

static void run_bg(const char *action, const char *name, const char *text)
{
	const char *argv[] = { CTL, action, name, NULL };
	GPid pid;

	if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD |
	                   G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, &pid, NULL))
		return;
	busy = 1;
	gtk_label_set_text(GTK_LABEL(status_label), text);
	gtk_label_set_text(GTK_LABEL(detail_label), "");
	update_buttons();
	g_child_watch_add(pid, on_done, (gpointer)action);
}

static void on_connect(GtkButton *b, gpointer d)
{
	const char *n = selected_name();

	(void)b; (void)d;
	if (n)
		run_bg("up", n, "Connecting…");
}

static void on_disconnect(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	run_bg("down", NULL, "Disconnecting…");
}

static void on_remove(GtkButton *b, gpointer d)
{
	const char *n = selected_name();
	GtkWidget *ask;
	int ok, answer;

	(void)b; (void)d;
	if (!n)
		return;
	ask = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
	                             GTK_BUTTONS_NONE, _("Remove the VPN \"%s\"?"), n);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask),
		_("To use it again you need its config file."));
	gtk_dialog_add_buttons(GTK_DIALOG(ask), _("Cancel"), GTK_RESPONSE_CANCEL,
	                       _("Remove"), GTK_RESPONSE_ACCEPT, NULL);
	answer = gtk_dialog_run(GTK_DIALOG(ask));
	gtk_widget_destroy(ask);
	if (answer == GTK_RESPONSE_ACCEPT) {
		g_free(ctl("remove", n, NULL, &ok));
		fill_list(NULL);
	}
}

/* The VPN's name from the file name: "Home VPN (2).conf" -> "Home-VPN-2" */
static void name_from_file(const char *path, char *name, size_t len)
{
	char *base = g_path_get_basename(path), *dot = strrchr(base, '.');
	size_t j = 0;

	if (dot && dot != base)
		*dot = '\0';
	for (char *p = base; *p && j + 1 < len && j < 15; p++) {
		if (isalnum((unsigned char)*p) || *p == '_')
			name[j++] = *p;
		else if (j > 0 && name[j - 1] != '-')
			name[j++] = '-';
	}
	while (j > 0 && name[j - 1] == '-')
		j--;
	name[j] = '\0';
	if (!name[0])
		g_strlcpy(name, "vpn", len);
	g_free(base);
}

static int exists(const char *name)
{
	int ok, found = 0;
	char *out = ctl("list", NULL, NULL, &ok), **names = g_strsplit(out ? out : "", "\n", -1);

	for (int i = 0; names[i]; i++) {
		char *tab = strchr(names[i], '\t');

		if (tab)
			*tab = '\0';
		found |= strcmp(names[i], name) == 0;
	}
	g_strfreev(names);
	g_free(out);
	return found;
}

/* The user name and password an OpenVPN file asks for (auth-user-pass).
 * Returns "user\npassword\n" (g_free it, wipe it first), or NULL if cancelled. */
static char *ask_credentials(const char *name, const char *user0, const char *pass0)
{
	GtkWidget *d, *box, *grid, *user, *pass, *show, *t;
	char *title, *result = NULL;

	d = gtk_dialog_new_with_buttons(_("VPN login"), GTK_WINDOW(win), GTK_DIALOG_MODAL,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("Save"), GTK_RESPONSE_OK, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	title = g_strdup_printf(_("The VPN \"%s\" asks for a user name and password:"), name);
	t = gtk_label_new(title);
	g_free(title);
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_label_set_line_wrap(GTK_LABEL(t), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
	gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
	user = gtk_entry_new();
	gtk_widget_set_hexpand(user, TRUE);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("User name")), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), user, 1, 0, 1, 1);
	pass = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(pass), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(pass), TRUE);
	gtk_widget_set_hexpand(pass, TRUE);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Password")), 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), pass, 1, 1, 1, 1);
	show = gtk_check_button_new_with_label(_("Show password"));
	g_object_bind_property(show, "active", pass, "visibility", 0);
	gtk_grid_attach(GTK_GRID(grid), show, 1, 2, 1, 1);
	/* from the file "auth-user-pass FILE" named, to check and save */
	if (user0)
		gtk_entry_set_text(GTK_ENTRY(user), user0);
	if (pass0)
		gtk_entry_set_text(GTK_ENTRY(pass), pass0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	gtk_widget_show_all(d);
	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		const char *u = gtk_entry_get_text(GTK_ENTRY(user)), *p = gtk_entry_get_text(GTK_ENTRY(pass));

		if (!u[0] || !p[0])
			continue;
		result = g_strdup_printf("%s\n%s\n", u, p);
		break;
	}
	gtk_widget_destroy(d);
	return result;
}

/* Does this OpenVPN file ask for a user name and password? */
static int wants_login(const char *content)
{
	char **lines = g_strsplit(content, "\n", -1);
	int found = 0;

	for (int i = 0; lines[i]; i++) {
		char *l = g_strstrip(lines[i]);

		found |= strcmp(l, "auth-user-pass") == 0;
	}
	g_strfreev(lines);
	return found;
}

/* Frees text that may hold a key or a password after overwriting it */
static void wipe_free(char *s)
{
	if (s) {
		memset(s, 0, strlen(s));
		g_free(s);
	}
}

static void wipe_strv(char **v)
{
	for (int i = 0; v && v[i]; i++)
		memset(v[i], 0, strlen(v[i]));
	g_strfreev(v);
}

/* The password of an OpenVPN config's private key. Returns it (wipe_free
 * it), or NULL if cancelled. */
static char *ask_key_password(const char *name)
{
	GtkWidget *d, *box, *pass, *show, *t;
	char *title, *result = NULL;

	d = gtk_dialog_new_with_buttons(_("Private key password"), GTK_WINDOW(win), GTK_DIALOG_MODAL,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("Save"), GTK_RESPONSE_OK, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	title = g_strdup_printf(_("The private key of the VPN \"%s\" is protected by a password. "
	                          "Enter it so the VPN can connect:"), name);
	t = gtk_label_new(title);
	g_free(title);
	gtk_label_set_xalign(GTK_LABEL(t), 0);
	gtk_label_set_line_wrap(GTK_LABEL(t), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
	gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	pass = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(pass), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(pass), TRUE);
	gtk_box_pack_start(GTK_BOX(box), pass, FALSE, FALSE, 0);
	show = gtk_check_button_new_with_label(_("Show password"));
	g_object_bind_property(show, "active", pass, "visibility", 0);
	gtk_box_pack_start(GTK_BOX(box), show, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	gtk_widget_show_all(d);
	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		const char *p = gtk_entry_get_text(GTK_ENTRY(pass));

		if (!p[0] || strchr(p, '\n'))
			continue;
		result = g_strdup_printf("%s\n", p);
		break;
	}
	gtk_widget_destroy(d);
	return result;
}

/* Is the private key in this (inlined) OpenVPN config protected by a
 * password? (PKCS#8 "ENCRYPTED PRIVATE KEY", or the old OpenSSL header) */
static int key_encrypted(const char *content)
{
	return strstr(content, "BEGIN ENCRYPTED PRIVATE KEY") ||
	       strstr(content, "Proc-Type: 4,ENCRYPTED");
}

/* Settings of an .ovpn file that name a file. my-vpnctl takes them only as
 * blocks (<ca> ... </ca>), so the file is read here, as the user. */
static const char *const file_keys[] = { "ca", "cert", "key", "tls-auth", "tls-crypt",
	"tls-crypt-v2", "extra-certs", "secret", "crl-verify", NULL };

/* The PEM parts of a file (-----BEGIN ... -----END ...): certificate files
 * often start with a readable dump of the certificate. NULL if it has none. */
static char *pem_parts(const char *text)
{
	GString *s = g_string_new(NULL);
	char **lines = g_strsplit(text, "\n", -1);
	int in = 0;

	for (int i = 0; lines[i]; i++) {
		char *l = g_strstrip(lines[i]);

		if (!in && g_str_has_prefix(l, "-----BEGIN "))
			in = 1;
		if (in) {
			g_string_append(s, l);
			g_string_append_c(s, '\n');
		}
		if (in && g_str_has_prefix(l, "-----END "))
			in = 0;
	}
	wipe_strv(lines);
	if (s->len == 0) {
		g_string_free(s, TRUE);
		return NULL;
	}
	return g_string_free(s, FALSE);
}

/* An OpenVPN config with the files it points to (ca ca.crt, tls-auth ta.key
 * 1, ...) put in as blocks, from the folder DIR the config is in. A user
 * name and password in the file "auth-user-pass FILE" names go to *user and
 * *pass (wipe_free them). Returns the new config (wipe_free it), or NULL
 * and *err (g_free it) when a file can't be used. */
static char *inline_files(const char *content, const char *dir, char **user, char **pass,
                          char **err)
{
	GString *out = g_string_new(NULL);
	char **lines = g_strsplit(content, "\n", -1);
	int block = 0;

	*user = *pass = *err = NULL;
	for (int i = 0; lines[i] && !*err; i++) {
		char *l = g_strstrip(lines[i]), **argv = NULL, *path, *text = NULL, *pem;
		int argc = 0, is_file;
		gsize n;

		if (block || (l[0] == '<' && l[1] != '/')) {     /* already a block */
			block = !g_str_has_prefix(l, "</");
			g_string_append_printf(out, "%s\n", l);
			continue;
		}
		if (!*l || *l == '#' || *l == ';' || !g_shell_parse_argv(l, &argc, &argv, NULL) ||
		    argc < 2) {
			g_string_append_printf(out, "%s\n", l);
			g_strfreev(argv);
			continue;
		}
		is_file = 0;
		for (int k = 0; file_keys[k]; k++)
			is_file |= strcasecmp(argv[0], file_keys[k]) == 0;
		if (strcasecmp(argv[0], "pkcs12") == 0) {
			*err = g_strdup(_("PKCS#12 files (.p12) are not supported. Ask your VPN service "
			                  "for the certificate and key as PEM files (.crt, .key)."));
		} else if (is_file && strcmp(argv[1], "[inline]") == 0) {
			/* "tls-auth [inline] 1": the block is elsewhere in the file */
			if (argc > 2)
				g_string_append_printf(out, "key-direction %s\n", argv[2]);
		} else if (is_file) {
			path = g_path_is_absolute(argv[1]) ? g_strdup(argv[1]) :
			       g_build_filename(dir, argv[1], NULL);
			if (!g_file_get_contents(path, &text, &n, NULL) || n > 64000) {
				*err = g_strdup_printf(_("The config points to the file %s (%s), which "
					"could not be read. Put it in the same folder as the config."),
					argv[1], argv[0]);
			} else if (!(pem = pem_parts(text))) {
				*err = g_strdup_printf(_("%s is not a key or certificate file (PEM)."),
				                       argv[1]);
			} else {
				g_string_append_printf(out, "<%s>\n%s</%s>\n", argv[0], pem, argv[0]);
				/* "tls-auth ta.key 1": the direction comes along */
				if (argc > 2 && (strcasecmp(argv[0], "tls-auth") == 0 ||
				                 strcasecmp(argv[0], "secret") == 0))
					g_string_append_printf(out, "key-direction %s\n", argv[2]);
				wipe_free(pem);
			}
			wipe_free(text);
			g_free(path);
		} else if (strcasecmp(argv[0], "auth-user-pass") == 0) {
			/* a file with the user name and the password: the login dialog
			 * shows them; the window saves them its own way */
			path = g_path_is_absolute(argv[1]) ? g_strdup(argv[1]) :
			       g_build_filename(dir, argv[1], NULL);
			if (g_file_get_contents(path, &text, &n, NULL) && n < 2048) {
				char **up = g_strsplit(text, "\n", 3);

				if (up[0] && up[1]) {
					*user = g_strdup(g_strstrip(up[0]));
					*pass = g_strdup(g_strstrip(up[1]));
				}
				wipe_strv(up);
			}
			wipe_free(text);
			g_free(path);
			g_string_append(out, "auth-user-pass\n");
		} else {
			g_string_append_printf(out, "%s\n", l);
		}
		g_strfreev(argv);
	}
	wipe_strv(lines);
	if (*err) {
		memset(out->str, 0, out->len);
		g_string_free(out, TRUE);
		wipe_free(*user);
		wipe_free(*pass);
		*user = *pass = NULL;
		return NULL;
	}
	return g_string_free(out, FALSE);
}

static void on_import(GtkButton *b, gpointer d)
{
	GtkWidget *fc;
	GtkFileFilter *f = gtk_file_filter_new();
	char *path = NULL, *content = NULL, *out, name[32], *login = NULL, *keypass = NULL;
	gsize len;
	int ok, ovpn;

	(void)b; (void)d;
	fc = gtk_file_chooser_dialog_new(_("Import a VPN config"), GTK_WINDOW(win),
		GTK_FILE_CHOOSER_ACTION_OPEN, _("Cancel"), GTK_RESPONSE_CANCEL,
		_("Import"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_filter_set_name(f, _("VPN configs (WireGuard .conf, OpenVPN .ovpn)"));
	gtk_file_filter_add_pattern(f, "*.conf");
	gtk_file_filter_add_pattern(f, "*.ovpn");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), f);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), g_get_home_dir());
	if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT)
		path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
	gtk_widget_destroy(fc);
	if (!path)
		return;
	if (!g_file_get_contents(path, &content, &len, NULL) || len > 60000) {
		message(GTK_MESSAGE_ERROR, _("Could not read that file."), NULL);
		g_free(path);
		g_free(content);
		return;
	}
	name_from_file(path, name, sizeof name);
	/* OpenVPN: .ovpn, or a .conf that is not a WireGuard one */
	ovpn = g_str_has_suffix(path, ".ovpn") || !strstr(content, "[Interface]");
	if (ovpn) {
		char *dir = g_path_get_dirname(path), *user, *pass, *err, *full;

		full = inline_files(content, dir, &user, &pass, &err);
		g_free(dir);
		wipe_free(content);
		content = full;
		if (content && strlen(content) > 120000) {
			wipe_free(content);
			content = NULL;
			err = g_strdup(_("The config and the files it points to are too big."));
		}
		if (!content) {
			message(GTK_MESSAGE_ERROR, _("Could not import this file."), err);
			g_free(err);
			g_free(path);
			return;
		}
		len = strlen(content);
		if (wants_login(content))
			login = ask_credentials(name, user, pass);
		wipe_free(user);
		wipe_free(pass);
		if (wants_login(content) && !login) {
			wipe_free(content);
			g_free(path);
			return;
		}
		if (key_encrypted(content) && !(keypass = ask_key_password(name))) {
			wipe_free(content);
			wipe_free(login);
			g_free(path);
			return;
		}
	}
	if (exists(name)) {
		GtkWidget *ask = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
			GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, _("Replace the VPN \"%s\"?"), name);
		int answer;

		gtk_dialog_add_buttons(GTK_DIALOG(ask), _("Cancel"), GTK_RESPONSE_CANCEL,
		                       _("Replace"), GTK_RESPONSE_ACCEPT, NULL);
		answer = gtk_dialog_run(GTK_DIALOG(ask));
		gtk_widget_destroy(ask);
		if (answer != GTK_RESPONSE_ACCEPT) {
			wipe_free(content);
			wipe_free(login);
			wipe_free(keypass);
			g_free(path);
			return;
		}
	}
	out = ctl(ovpn ? "import-ovpn" : "import", name, content, &ok);
	memset(content, 0, len);          /* the private key */
	g_free(content);
	if (ok && login) {
		char *junk = ctl("auth", name, login, &ok);

		g_free(junk);
		if (!ok) {
			g_free(out);
			out = g_strdup(_("The user name and password could not be saved."));
		}
	}
	if (ok && keypass) {
		char *junk = ctl("keypass", name, keypass, &ok);

		g_free(junk);
		if (!ok) {
			g_free(out);
			out = g_strdup(_("The password of the private key could not be saved."));
		}
	}
	wipe_free(login);
	wipe_free(keypass);
	if (ok) {
		fill_list(name);
		if (out && out[0])                 /* what was left out */
			message(GTK_MESSAGE_INFO, _("VPN imported."), out);
	} else {
		message(GTK_MESSAGE_ERROR, _("Could not import this file."), out);
	}
	g_free(out);
	g_free(path);
}

static void on_row(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)r; (void)d;
	update_buttons();
}

static void on_row_activated(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)r; (void)d;
	if (gtk_widget_get_sensitive(btn_connect))
		on_connect(NULL, NULL);
}

static GtkWidget *button(const char *text, GCallback cb)
{
	GtkWidget *b = gtk_button_new_with_label(text);

	g_signal_connect(b, "clicked", cb, NULL);
	return b;
}

int main(int argc, char **argv)
{
	GtkWidget *box, *scroll, *row;
	int ok;
	char *out;

	i18n_init();
	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), _("VPN"));
	gtk_window_set_default_size(GTK_WINDOW(win), 480, 400);
	gtk_window_set_icon_name(GTK_WINDOW(win), "network-vpn");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(win), box);
	status_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status_label), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "title");
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);
	detail_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(detail_label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(detail_label), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(detail_label), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), detail_label, FALSE, FALSE, 0);

	list = gtk_list_box_new();
	/* a click selects (for Remove); a double-click connects */
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), FALSE);
	g_signal_connect(list, "row-selected", G_CALLBACK(on_row), NULL);
	g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	btn_connect = button(_("Connect"), G_CALLBACK(on_connect));
	gtk_style_context_add_class(gtk_widget_get_style_context(btn_connect), "suggested-action");
	btn_disconnect = button(_("Disconnect"), G_CALLBACK(on_disconnect));
	btn_remove = button(_("Remove…"), G_CALLBACK(on_remove));
	gtk_box_pack_start(GTK_BOX(row), button(_("Import…"), G_CALLBACK(on_import)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), btn_remove, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_connect, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_disconnect, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	/* Not allowed (not in netdev): say so instead of an empty window */
	out = ctl("list", NULL, NULL, &ok);
	if (!ok) {
		gtk_label_set_text(GTK_LABEL(status_label), _("VPN not available"));
		gtk_label_set_text(GTK_LABEL(detail_label), out && out[0] ? out :
			_("my-vpnctl is missing."));
		gtk_widget_set_sensitive(row, FALSE);
	} else {
		fill_list(NULL);
		refresh_status(NULL);
		g_timeout_add_seconds(2, refresh_status, NULL);
	}
	g_free(out);
	gtk_main();
	return 0;
}
