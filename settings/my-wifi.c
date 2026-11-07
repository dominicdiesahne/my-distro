/*
 * my-wifi: the Wi-Fi window of my-distro (GTK 3). The taskbar's Wi-Fi bars
 * and the Start menu open it.
 *
 * wpa_supplicant does the real work (wifi-start starts it when an adapter
 * appears). This window talks to it with wpa_cli, its command-line tool:
 *   scan / scan_results      which networks are around
 *   list_networks            the saved ones (/etc/wpa_supplicant.conf)
 *   add_network, set_network, select_network, save_config   connect to a new one
 *   remove_network           forget one
 *   status                   connected? to which network, which IP address
 * Members of the group netdev may do this without root (every user that the
 * setup or "Users and system" makes is one).
 *
 * Besides networks with one password (WPA2/WPA3 Personal) it connects to
 * networks with a user name and password (WPA2/WPA3 Enterprise, e.g.
 * eduroam at schools and universities) and, with "Other network…", to hidden
 * networks that don't show their name.
 */
#include <ctype.h>
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CTRL_DIR "/run/wpa_supplicant"

struct net {
	char ssid[33 * 4];     /* as wpa_cli shows it (\xNN for odd bytes) */
	int signal;            /* dBm, e.g. -55 */
	int secured, sae_only;
	int enterprise;        /* user name + password (802.1X / EAP), e.g. eduroam */
	int in_range;          /* 0: saved, but not found by the last scan (or hidden) */
	int saved_id;          /* -1: not saved */
	int current;
};

#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

static char iface[32];
static GtkWidget *win, *status_label, *list, *btn_connect, *btn_forget, *btn_disconnect;
static struct net nets[64];
static int nnets;
static guint poll_id;
static int connect_tries, new_id = -1;
static gint64 keep_message_until;   /* an error stays readable until then */

/* --- Talking to wpa_supplicant ----------------------------------------------- */

/* Runs "wpa_cli -i IFACE ARGS..." and returns what it printed (g_free it),
 * or NULL. No shell: names and passwords are passed as they are. */
static char *wpa(const char *a0, const char *a1, const char *a2, const char *a3)
{
	/* full path: /usr/sbin is not in a normal user's PATH */
	const char *argv[] = { "/usr/sbin/wpa_cli", "-p", CTRL_DIR, "-i", iface, a0, a1, a2, a3, NULL };
	char *out = NULL;
	int st;

	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, &st, NULL))
		return NULL;
	if (!g_spawn_check_wait_status(st, NULL) || !out || strstr(out, "Failed to connect")) {
		g_free(out);
		return NULL;
	}
	return out;
}

static int wpa_ok(const char *a0, const char *a1, const char *a2, const char *a3)
{
	char *out = wpa(a0, a1, a2, a3);
	int ok = out && strncmp(out, "OK", 2) == 0;

	g_free(out);
	return ok;
}

/* The Wi-Fi interface: one with /sys/class/net/NAME/phy80211. With several
 * (laptop Wi-Fi and a USB stick), the first that wpa_supplicant manages. */
static int find_iface(void)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	char path[300];

	while (d && (e = readdir(d))) {
		snprintf(path, sizeof path, "/sys/class/net/%s/phy80211", e->d_name);
		if (e->d_name[0] == '.' || !g_file_test(path, G_FILE_TEST_EXISTS))
			continue;
		snprintf(path, sizeof path, CTRL_DIR "/%s", e->d_name);
		if (!iface[0] || access(path, F_OK) == 0)
			g_strlcpy(iface, e->d_name, sizeof iface);
		if (access(path, F_OK) == 0)
			break;
	}
	if (d)
		closedir(d);
	return iface[0] != '\0';
}

/* wpa_cli shows odd bytes in names as \xNN, \\, \" ...: back to the bytes,
 * then as hex, which is how set_network takes any name safely. */
static char *ssid_hex(const char *shown)
{
	GString *hex = g_string_new(NULL);

	for (const char *p = shown; *p; p++) {
		unsigned char c = *p;

		if (*p == '\\' && p[1] == 'x' && isxdigit((unsigned char)p[2]) &&
		    isxdigit((unsigned char)p[3])) {
			c = (unsigned char)strtol((char[]){ p[2], p[3], 0 }, NULL, 16);
			p += 3;
		} else if (*p == '\\' && p[1]) {
			p++;
			c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == 'e' ? 27 : *p;
		}
		g_string_append_printf(hex, "%02x", c);
	}
	return g_string_free(hex, FALSE);
}

/* Any text as hex: wpa_supplicant takes unquoted hex for its text settings
 * (name, user name, password, file paths), so quotes, spaces and other odd
 * characters in them can't break the command. */
static char *text_hex(const char *s)
{
	GString *hex = g_string_new(NULL);

	for (const unsigned char *p = (const unsigned char *)s; *p; p++)
		g_string_append_printf(hex, "%02x", *p);
	return g_string_free(hex, FALSE);
}

/* set_network with a hex-encoded text value; wipes the hex copy afterwards */
static int set_text(const char *id, const char *key, const char *value)
{
	char *hex = text_hex(value);
	int ok = wpa_ok("set_network", id, key, hex);

	memset(hex, 0, strlen(hex));
	g_free(hex);
	return ok;
}

static void status_text(char *buf, size_t len, int *connected)
{
	char *out = wpa("status", NULL, NULL, NULL), *ssid = NULL, *ip = NULL, *state = NULL;
	char **lines;

	*connected = 0;
	if (!out) {
		snprintf(buf, len, "Wi-Fi is not running yet. If you just plugged in an "
		         "adapter, wait a moment.");
		return;
	}
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		if (g_str_has_prefix(lines[i], "ssid="))
			ssid = lines[i] + 5;
		else if (g_str_has_prefix(lines[i], "ip_address="))
			ip = lines[i] + 11;
		else if (g_str_has_prefix(lines[i], "wpa_state="))
			state = lines[i] + 10;
	}
	if (state && strcmp(state, "COMPLETED") == 0) {
		*connected = 1;
		snprintf(buf, len, "Connected to %s%s%s", ssid ? ssid : "?",
		         ip ? ", address " : " (getting an address…)", ip ? ip : "");
	} else if (state && (strstr(state, "ASSOCIAT") || strstr(state, "HANDSHAKE") ||
	                     strstr(state, "AUTHENTICATING"))) {
		snprintf(buf, len, "Connecting…");
	} else {
		snprintf(buf, len, "Not connected.");
	}
	g_strfreev(lines);
	g_free(out);
}

/* --- The list --------------------------------------------------------------------- */

static int by_signal(const void *a, const void *b)
{
	return ((const struct net *)b)->signal - ((const struct net *)a)->signal;
}

static const char *bars(int dbm)
{
	return dbm >= -55 ? "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86\xe2\x96\x88" :   /* ▂▄▆█ */
	       dbm >= -67 ? "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86 " :
	       dbm >= -78 ? "\xe2\x96\x82\xe2\x96\x84  " : "\xe2\x96\x82   ";
}

static void read_networks(void)
{
	char *res = wpa("scan_results", NULL, NULL, NULL);
	char *saved = wpa("list_networks", NULL, NULL, NULL);
	char **lines;

	nnets = 0;
	if (res) {
		lines = g_strsplit(res, "\n", -1);
		/* bssid  frequency  signal  flags  ssid (after a header line) */
		for (int i = 1; lines[i] && nnets < (int)G_N_ELEMENTS(nets); i++) {
			char **f = g_strsplit(lines[i], "\t", 5);
			int j;

			if (g_strv_length(f) == 5 && f[4][0]) {      /* hidden networks: no name */
				for (j = 0; j < nnets && strcmp(nets[j].ssid, f[4]) != 0; j++)
					;
				if (j == nnets) {                       /* new name */
					memset(&nets[j], 0, sizeof nets[j]);
					g_strlcpy(nets[j].ssid, f[4], sizeof nets[j].ssid);
					nets[j].signal = -200;
					nets[j].saved_id = -1;
					nets[j].in_range = 1;
					nnets++;
				}
				if (atoi(f[2]) > nets[j].signal)       /* strongest access point */
					nets[j].signal = atoi(f[2]);
				nets[j].secured = strstr(f[3], "WPA") || strstr(f[3], "RSN") ||
				                  strstr(f[3], "WEP") || strstr(f[3], "SAE");
				nets[j].sae_only = strstr(f[3], "SAE") && !strstr(f[3], "PSK");
				/* e.g. [WPA2-EAP-CCMP]: user name and password */
				nets[j].enterprise = strstr(f[3], "-EAP") != NULL;
			}
			g_strfreev(f);
		}
		g_strfreev(lines);
	}
	if (saved) {
		lines = g_strsplit(saved, "\n", -1);
		/* id  ssid  bssid  flags */
		for (int i = 1; lines[i]; i++) {
			char **f = g_strsplit(lines[i], "\t", 4);
			int j, found = 0;

			if (g_strv_length(f) >= 2) {
				for (j = 0; j < nnets; j++)
					if (strcmp(nets[j].ssid, f[1]) == 0) {
						nets[j].saved_id = atoi(f[0]);
						nets[j].current = f[3] && strstr(f[3], "CURRENT");
						found = 1;
					}
				/* Saved, but not seen: out of range, or a hidden network
				 * (those don't send their name). Listed, so it can be
				 * chosen or forgotten. */
				if (!found && f[1][0] && nnets < (int)G_N_ELEMENTS(nets)) {
					j = nnets++;
					memset(&nets[j], 0, sizeof nets[j]);
					g_strlcpy(nets[j].ssid, f[1], sizeof nets[j].ssid);
					nets[j].signal = -200;
					nets[j].saved_id = atoi(f[0]);
					nets[j].current = f[3] && strstr(f[3], "CURRENT");
				}
			}
			g_strfreev(f);
		}
		g_strfreev(lines);
	}
	qsort(nets, nnets, sizeof nets[0], by_signal);
	g_free(res);
	g_free(saved);
}

static struct net *selected(void)
{
	GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));

	return r ? &nets[gtk_list_box_row_get_index(r)] : NULL;
}

static void update_buttons(void)
{
	struct net *n = selected();
	int connected;
	char text[200];

	status_text(text, sizeof text, &connected);
	gtk_label_set_text(GTK_LABEL(status_label), text);
	gtk_widget_set_sensitive(btn_connect, n && !n->current);
	gtk_widget_set_sensitive(btn_forget, n && n->saved_id >= 0);
	gtk_widget_set_sensitive(btn_disconnect, connected);
}

static void fill_list(void)
{
	GList *old = gtk_container_get_children(GTK_CONTAINER(list));

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
	for (int i = 0; i < nnets; i++) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		GtkWidget *name = gtk_label_new(NULL), *info = gtk_label_new(NULL);
		char *m = g_markup_printf_escaped(nets[i].current ? "<b>%s</b>" : "%s", nets[i].ssid);
		char *t;

		if (!nets[i].in_range)
			t = g_strdup_printf("%s%s", nets[i].current ? "connected   " : "saved   ",
			                    nets[i].current ? "hidden" : "not found");
		else
			t = g_strdup_printf("%s%s%s", nets[i].current ? "connected   " :
			                    nets[i].saved_id >= 0 ? "saved   " : "",
			                    nets[i].enterprise ? "user name   " :
			                    nets[i].secured ? "secured   " : "open   ",
			                    bars(nets[i].signal));

		gtk_label_set_markup(GTK_LABEL(name), m);
		gtk_label_set_xalign(GTK_LABEL(name), 0);
		gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
		gtk_label_set_text(GTK_LABEL(info), t);
		gtk_style_context_add_class(gtk_widget_get_style_context(info), "dim-label");
		g_object_set(row, "margin", 8, NULL);
		gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
		gtk_box_pack_end(GTK_BOX(row), info, FALSE, FALSE, 0);
		gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
		g_free(m);
		g_free(t);
	}
	gtk_widget_show_all(list);
	update_buttons();
}

static int scan_retries;

static gboolean scan_done(gpointer d)
{
	int seen = 0;

	(void)d;
	read_networks();
	for (int i = 0; i < nnets; i++)
		seen += nets[i].in_range;
	/* the first scan after Wi-Fi started can come back empty: try again */
	if (seen == 0 && scan_retries++ < 3) {
		wpa_ok("scan", NULL, NULL, NULL);
		return G_SOURCE_CONTINUE;
	}
	scan_retries = 0;
	fill_list();
	return G_SOURCE_REMOVE;
}

/* Keeps the status line current (e.g. when DHCP has given an address) */
static gboolean refresh_status(gpointer d)
{
	int connected;
	char text[200];

	(void)d;
	if (!poll_id && g_get_monotonic_time() > keep_message_until) {
		status_text(text, sizeof text, &connected);
		gtk_label_set_text(GTK_LABEL(status_label), text);
		gtk_widget_set_sensitive(btn_disconnect, connected);
	}
	return G_SOURCE_CONTINUE;
}

static void on_scan(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	if (!wpa_ok("scan", NULL, NULL, NULL)) {    /* busy scanning already is fine */
		read_networks();
		fill_list();
	}
	gtk_label_set_text(GTK_LABEL(status_label), "Looking for networks…");
	g_timeout_add(3500, scan_done, NULL);
}

/* --- Connecting ------------------------------------------------------------------ */

/* After select_network: wait until connected, or give up after ~20 seconds
 * (wrong password). A network added just now is forgotten again then. */
static gboolean poll_connect(gpointer d)
{
	int connected;
	char text[200];

	(void)d;
	status_text(text, sizeof text, &connected);
	gtk_label_set_text(GTK_LABEL(status_label), text);
	/* select_network switched all other saved networks off. Back on, so
	 * they connect by themselves again later (at home, at school...) and
	 * "disabled" isn't saved for them. */
	if (connected) {
		wpa_ok("enable_network", "all", NULL, NULL);
		if (new_id >= 0)
			wpa_ok("save_config", NULL, NULL, NULL);
		new_id = -1;
		read_networks();
		fill_list();
		poll_id = 0;
		return G_SOURCE_REMOVE;
	}
	if (++connect_tries < 20)
		return G_SOURCE_CONTINUE;
	if (new_id >= 0) {
		char id[16];

		snprintf(id, sizeof id, "%d", new_id);
		wpa_ok("remove_network", id, NULL, NULL);
		new_id = -1;
	}
	wpa_ok("enable_network", "all", NULL, NULL);   /* the previous one may connect again */
	read_networks();
	fill_list();                      /* sets the status line too: say why after it */
	gtk_label_set_text(GTK_LABEL(status_label),
		"Could not connect. Wrong password, or the network is too far away?");
	keep_message_until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	poll_id = 0;
	return G_SOURCE_REMOVE;
}

static void wait_for_connection(void)
{
	connect_tries = 0;
	gtk_label_set_text(GTK_LABEL(status_label), "Connecting…");
	if (poll_id)
		g_source_remove(poll_id);
	poll_id = g_timeout_add(1000, poll_connect, NULL);
}

/* Asks for the password of a secured network. NULL = cancelled. */
static char *ask_password(const char *ssid)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Wi-Fi password", GTK_WINDOW(win),
		GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "Connect", GTK_RESPONSE_OK, NULL);
	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	GtkWidget *entry = gtk_entry_new(), *show = gtk_check_button_new_with_label("Show password");
	GtkWidget *err = gtk_label_new("");
	char *text = g_strdup_printf("Password for \"%s\":", ssid), *pw = NULL;

	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new(text), FALSE, FALSE, 0);
	gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	g_object_bind_property(show, "active", entry, "visibility", 0);
	gtk_box_pack_start(GTK_BOX(box), show, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), err, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	gtk_widget_show_all(d);
	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		const char *p = gtk_entry_get_text(GTK_ENTRY(entry));
		size_t n = strlen(p);

		if (n >= 8 && n <= 63 && !strchr(p, '"')) {
			pw = g_strdup(p);
			break;
		}
		gtk_label_set_text(GTK_LABEL(err), "Wi-Fi passwords have 8 to 63 characters.");
	}
	gtk_widget_destroy(d);
	g_free(text);
	return pw;
}

/* --- Networks with a user name (eduroam), hidden networks -------------------- */

enum { SEC_OPEN, SEC_PASSWORD, SEC_LOGIN };

/* What was typed in ask_login() */
struct login {
	char *ssid;           /* "Other network…" only: the name typed in */
	int security;         /* SEC_* */
	char *user, *password, *anon, *domain, *ca;
	int method;           /* 0 PEAP + MSCHAPv2, 1 TTLS + PAP, 2 TTLS + MSCHAPv2 */
};

static void free_login(struct login *l)
{
	if (l->password)
		memset(l->password, 0, strlen(l->password));
	g_free(l->ssid);
	g_free(l->user);
	g_free(l->password);
	g_free(l->anon);
	g_free(l->domain);
	g_free(l->ca);
	memset(l, 0, sizeof *l);
}

static GtkWidget *lg_security, *lg_user_row[2], *lg_pass_row[2], *lg_advanced, *lg_hint;

static void grid_row(GtkWidget *grid, int y, const char *caption, GtkWidget *w, GtkWidget **row)
{
	GtkWidget *l = gtk_label_new(caption);

	gtk_label_set_xalign(GTK_LABEL(l), 1);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_grid_attach(GTK_GRID(grid), l, 0, y, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), w, 1, y, 1, 1);
	if (row) {
		row[0] = l;
		row[1] = w;
	}
}

static void show_row(GtkWidget **row, int show)
{
	gtk_widget_set_visible(row[0], show);
	gtk_widget_set_visible(row[1], show);
}

/* Shows the fields the chosen kind of security needs */
static void on_security(GtkComboBox *c, gpointer d)
{
	int s = gtk_combo_box_get_active(c);

	(void)d;
	show_row(lg_user_row, s == SEC_LOGIN);
	show_row(lg_pass_row, s != SEC_OPEN);
	gtk_widget_set_visible(lg_advanced, s == SEC_LOGIN);
	gtk_widget_set_visible(lg_hint, s == SEC_LOGIN);
	/* as small as what is shown (it grows by itself, but doesn't shrink) */
	gtk_window_resize(GTK_WINDOW(gtk_widget_get_toplevel(lg_security)), 1, 1);
}

/*
 * Asks how to log in. ssid = the network's name, or NULL for "Other
 * network…" (then the name and the kind of security are asked too).
 * security: what the network said it uses (ignored for NULL).
 * Returns 0 if cancelled.
 */
static int ask_login(const char *ssid, int security, struct login *l)
{
	GtkWidget *d, *box, *grid, *name = NULL, *user, *pass, *show, *err, *adv;
	GtkWidget *method, *anon, *domain, *ca;
	char *title;
	int y = 0, ok = 0;

	memset(l, 0, sizeof *l);
	d = gtk_dialog_new_with_buttons(ssid ? "Connect to Wi-Fi" : "Other network", GTK_WINDOW(win),
		GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL, "Connect", GTK_RESPONSE_OK, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_box_set_spacing(GTK_BOX(box), 10);

	title = ssid ? g_strdup_printf("Log in to \"%s\":", ssid)
	             : g_strdup("A network that doesn't show its name (hidden). Type it exactly, "
	                        "with upper and lower case:");
	{
		GtkWidget *t = gtk_label_new(title);

		gtk_label_set_xalign(GTK_LABEL(t), 0);
		gtk_label_set_line_wrap(GTK_LABEL(t), TRUE);
		gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
		gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
	}
	g_free(title);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

	lg_security = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(lg_security), "None (open network)");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(lg_security), "Password (WPA2/WPA3 Personal)");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(lg_security),
	                               "User name and password (eduroam, WPA Enterprise)");
	gtk_combo_box_set_active(GTK_COMBO_BOX(lg_security), ssid ? security : SEC_PASSWORD);
	if (!ssid) {
		name = gtk_entry_new();
		gtk_entry_set_max_length(GTK_ENTRY(name), 32);
		grid_row(grid, y++, "Network name", name, NULL);
		grid_row(grid, y++, "Security", lg_security, NULL);
	}
	user = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(user), "e.g. name@school.ac.at");
	grid_row(grid, y++, "User name", user, lg_user_row);
	pass = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(pass), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(pass), TRUE);
	grid_row(grid, y++, "Password", pass, lg_pass_row);
	show = gtk_check_button_new_with_label("Show password");
	g_object_bind_property(show, "active", pass, "visibility", 0);
	gtk_grid_attach(GTK_GRID(grid), show, 1, y++, 1, 1);

	/* For user name networks: how the login is done, and how we make sure
	 * the server is the real one (or someone could collect the password). */
	adv = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(adv), 8);
	gtk_grid_set_column_spacing(GTK_GRID(adv), 10);
	g_object_set(adv, "margin-top", 8, NULL);
	method = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(method), "PEAP + MSCHAPv2 (most, eduroam)");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(method), "TTLS + PAP");
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(method), "TTLS + MSCHAPv2");
	gtk_combo_box_set_active(GTK_COMBO_BOX(method), 0);
	grid_row(adv, 0, "Method", method, NULL);
	anon = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(anon), "optional, e.g. anonymous@school.ac.at");
	grid_row(adv, 1, "Anonymous identity", anon, NULL);
	domain = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(domain), "e.g. school.ac.at");
	grid_row(adv, 2, "Server domain", domain, NULL);
	ca = gtk_file_chooser_button_new("CA certificate", GTK_FILE_CHOOSER_ACTION_OPEN);
	grid_row(adv, 3, "CA certificate", ca, NULL);
	lg_advanced = gtk_expander_new("Settings from your school or university");
	gtk_container_add(GTK_CONTAINER(lg_advanced), adv);
	gtk_box_pack_start(GTK_BOX(box), lg_advanced, FALSE, FALSE, 0);
	lg_hint = gtk_label_new("Your school's Wi-Fi page lists these. With a server domain "
		"(and the CA certificate if they give you one), my-distro checks that it "
		"really is their server before it sends your password.");
	gtk_label_set_xalign(GTK_LABEL(lg_hint), 0);
	gtk_label_set_line_wrap(GTK_LABEL(lg_hint), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(lg_hint), 50);
	gtk_style_context_add_class(gtk_widget_get_style_context(lg_hint), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), lg_hint, FALSE, FALSE, 0);

	err = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(err), 0);
	gtk_box_pack_start(GTK_BOX(box), err, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	g_signal_connect(lg_security, "changed", G_CALLBACK(on_security), NULL);
	gtk_widget_show_all(d);
	on_security(GTK_COMBO_BOX(lg_security), NULL);
	gtk_widget_grab_focus(name ? name : security == SEC_LOGIN ? user : pass);

	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		int s = gtk_combo_box_get_active(GTK_COMBO_BOX(lg_security));
		const char *p = gtk_entry_get_text(GTK_ENTRY(pass));
		const char *u = gtk_entry_get_text(GTK_ENTRY(user));
		size_t n = strlen(p);

		if (name && !gtk_entry_get_text(GTK_ENTRY(name))[0]) {
			gtk_label_set_text(GTK_LABEL(err), "Type the network's name.");
			continue;
		}
		if (s == SEC_PASSWORD && (n < 8 || n > 63 || strchr(p, '"'))) {
			gtk_label_set_text(GTK_LABEL(err), "Wi-Fi passwords have 8 to 63 characters.");
			continue;
		}
		if (s == SEC_LOGIN && (!u[0] || !p[0])) {
			gtk_label_set_text(GTK_LABEL(err), "Type your user name and password.");
			continue;
		}
		l->security = s;
		l->ssid = name ? g_strdup(gtk_entry_get_text(GTK_ENTRY(name))) : NULL;
		l->password = s != SEC_OPEN ? g_strdup(p) : NULL;
		if (s == SEC_LOGIN) {
			l->user = g_strdup(u);
			l->anon = g_strdup(gtk_entry_get_text(GTK_ENTRY(anon)));
			l->domain = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(domain))));
			l->ca = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(ca));
			l->method = gtk_combo_box_get_active(GTK_COMBO_BOX(method));
		}
		ok = 1;
		break;
	}
	gtk_widget_destroy(d);
	return ok;
}

/* Adds a network to wpa_supplicant and connects. hex_ssid: the name as hex. */
static void add_and_connect(const char *hex_ssid, const struct login *l, int sae_only, int hidden)
{
	char *out, *quoted, id[16];

	out = wpa("add_network", NULL, NULL, NULL);
	if (!out || !isdigit((unsigned char)out[0])) {
		gtk_label_set_text(GTK_LABEL(status_label), "Could not add the network.");
		g_free(out);
		return;
	}
	snprintf(id, sizeof id, "%d", atoi(out));
	g_free(out);
	new_id = atoi(id);

	wpa_ok("set_network", id, "ssid", hex_ssid);
	if (hidden)                       /* ask for it by name: it doesn't announce itself */
		wpa_ok("set_network", id, "scan_ssid", "1");
	switch (l->security) {
	case SEC_OPEN:
		wpa_ok("set_network", id, "key_mgmt", "NONE");
		break;
	case SEC_PASSWORD:
		quoted = g_strdup_printf("\"%s\"", l->password);
		if (sae_only) {                         /* WPA3 only */
			wpa_ok("set_network", id, "key_mgmt", "SAE");
			wpa_ok("set_network", id, "ieee80211w", "2");
			wpa_ok("set_network", id, "sae_password", quoted);
		} else {
			/* WPA2, or WPA2/WPA3 mixed. Hidden: we can't see which, so
			 * allow both (WPA3 uses the same password). */
			if (hidden) {
				wpa_ok("set_network", id, "key_mgmt", "WPA-PSK SAE");
				wpa_ok("set_network", id, "ieee80211w", "1");
			}
			wpa_ok("set_network", id, "psk", quoted);
		}
		memset(quoted, 0, strlen(quoted));
		g_free(quoted);
		break;
	case SEC_LOGIN: {
		static const char *eap[] = { "PEAP", "TTLS", "TTLS" };
		static const char *phase2[] = { "auth=MSCHAPV2", "auth=PAP", "auth=MSCHAPV2" };

		wpa_ok("set_network", id, "key_mgmt", "WPA-EAP WPA-EAP-SHA256");
		wpa_ok("set_network", id, "ieee80211w", "1");
		wpa_ok("set_network", id, "eap", eap[l->method]);
		set_text(id, "phase2", phase2[l->method]);
		set_text(id, "identity", l->user);
		set_text(id, "password", l->password);
		if (l->anon && l->anon[0])
			set_text(id, "anonymous_identity", l->anon);
		/* The server's certificate: from the given file, or else (with a
		 * domain) from the usual certificate authorities, and its name must
		 * end in the domain. Without both it isn't checked. */
		if (l->ca && l->ca[0])
			set_text(id, "ca_cert", l->ca);
		else if (l->domain && l->domain[0])
			set_text(id, "ca_cert", CA_BUNDLE);
		if (l->domain && l->domain[0])
			set_text(id, "domain_suffix_match", l->domain);
		break;
	}
	}
	if (wpa_ok("select_network", id, NULL, NULL))
		wait_for_connection();
}

static void on_connect(GtkButton *b, gpointer d)
{
	struct net *n = selected();
	struct login l;
	char *hex, id[16];

	(void)b; (void)d;
	if (!n || n->current)
		return;
	if (n->saved_id >= 0) {                     /* known: just switch to it */
		snprintf(id, sizeof id, "%d", n->saved_id);
		new_id = -1;
		if (wpa_ok("select_network", id, NULL, NULL))
			wait_for_connection();
		return;
	}
	memset(&l, 0, sizeof l);
	if (n->enterprise) {
		if (!ask_login(n->ssid, SEC_LOGIN, &l))
			return;
	} else if (n->secured) {
		if (!(l.password = ask_password(n->ssid)))
			return;
		l.security = SEC_PASSWORD;
	}
	hex = ssid_hex(n->ssid);
	add_and_connect(hex, &l, n->sae_only, 0);
	g_free(hex);
	free_login(&l);
}

/* "Other network…": a hidden network, typed in by name */
static void on_other(GtkButton *b, gpointer d)
{
	struct login l;
	char *hex;

	(void)b; (void)d;
	if (!ask_login(NULL, SEC_PASSWORD, &l))
		return;
	hex = text_hex(l.ssid);
	add_and_connect(hex, &l, 0, 1);
	g_free(hex);
	free_login(&l);
}

static void on_forget(GtkButton *b, gpointer d)
{
	struct net *n = selected();
	char id[16];

	(void)b; (void)d;
	if (!n || n->saved_id < 0)
		return;
	snprintf(id, sizeof id, "%d", n->saved_id);
	wpa_ok("remove_network", id, NULL, NULL);
	wpa_ok("save_config", NULL, NULL, NULL);
	read_networks();
	fill_list();
}

static void on_disconnect(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	wpa_ok("disconnect", NULL, NULL, NULL);
	read_networks();
	fill_list();
}

static void on_row(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)r; (void)d;
	update_buttons();
}

static void on_row_activated(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)r; (void)d;
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
	char sock[300];

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Wi-Fi");
	gtk_window_set_default_size(GTK_WINDOW(win), 560, 480);
	gtk_window_set_icon_name(GTK_WINDOW(win), "network-wireless");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(win), box);
	status_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status_label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

	if (!find_iface()) {
		gtk_label_set_text(GTK_LABEL(status_label),
			"No Wi-Fi adapter found.\n\nThis computer has none, or my-distro has no "
			"driver for it (in a terminal, lspci -k shows the devices and their drivers). "
			"A network cable works without setting anything up.");
		gtk_widget_show_all(win);
		gtk_main();
		return 0;
	}

	list = gtk_list_box_new();
	/* a click selects (for Forget); a double-click connects */
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), FALSE);
	g_signal_connect(list, "row-selected", G_CALLBACK(on_row), NULL);
	g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	btn_connect = button("Connect", G_CALLBACK(on_connect));
	gtk_style_context_add_class(gtk_widget_get_style_context(btn_connect), "suggested-action");
	btn_forget = button("Forget", G_CALLBACK(on_forget));
	btn_disconnect = button("Disconnect", G_CALLBACK(on_disconnect));
	gtk_box_pack_start(GTK_BOX(row), button("Search again", G_CALLBACK(on_scan)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), button("Other network…", G_CALLBACK(on_other)), FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_connect, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_forget, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_disconnect, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	gtk_widget_set_sensitive(btn_connect, FALSE);    /* until a network is chosen */
	gtk_widget_set_sensitive(btn_forget, FALSE);
	gtk_widget_set_sensitive(btn_disconnect, FALSE);
	snprintf(sock, sizeof sock, CTRL_DIR "/%s", iface);
	if (access(sock, F_OK) != 0)
		gtk_label_set_text(GTK_LABEL(status_label), "Wi-Fi is starting…");
	else if (access(sock, W_OK) != 0)
		gtk_label_set_text(GTK_LABEL(status_label), "Your user may not change Wi-Fi "
			"yet (it is not in the group netdev). Log out and in again; if that "
			"doesn't help, as root: addgroup YOUR-NAME netdev, then log in again.");
	else {
		on_scan(NULL, NULL);
		g_timeout_add_seconds(3, refresh_status, NULL);
	}
	gtk_main();
	return 0;
}
