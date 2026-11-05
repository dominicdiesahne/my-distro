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
	int saved_id;          /* -1: not saved */
	int current;
};

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
					nnets++;
				}
				if (atoi(f[2]) > nets[j].signal)       /* strongest access point */
					nets[j].signal = atoi(f[2]);
				nets[j].secured = strstr(f[3], "WPA") || strstr(f[3], "RSN") ||
				                  strstr(f[3], "WEP") || strstr(f[3], "SAE");
				nets[j].sae_only = strstr(f[3], "SAE") && !strstr(f[3], "PSK");
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

			if (g_strv_length(f) >= 2)
				for (int j = 0; j < nnets; j++)
					if (strcmp(nets[j].ssid, f[1]) == 0) {
						nets[j].saved_id = atoi(f[0]);
						nets[j].current = f[3] && strstr(f[3], "CURRENT");
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
		char *t = g_strdup_printf("%s%s%s", nets[i].current ? "connected   " :
		                          nets[i].saved_id >= 0 ? "saved   " : "",
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
	(void)d;
	read_networks();
	/* the first scan after Wi-Fi started can come back empty: try again */
	if (nnets == 0 && scan_retries++ < 3) {
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
	if (connected) {
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

static void on_connect(GtkButton *b, gpointer d)
{
	struct net *n = selected();
	char *out, *hex, *pw = NULL, *quoted, id[16];

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
	if (n->secured && !(pw = ask_password(n->ssid)))
		return;

	out = wpa("add_network", NULL, NULL, NULL);
	if (!out || !isdigit((unsigned char)out[0])) {
		gtk_label_set_text(GTK_LABEL(status_label), "Could not add the network.");
		g_free(out);
		g_free(pw);
		return;
	}
	snprintf(id, sizeof id, "%d", atoi(out));
	g_free(out);
	new_id = atoi(id);

	hex = ssid_hex(n->ssid);
	wpa_ok("set_network", id, "ssid", hex);
	g_free(hex);
	if (!pw) {
		wpa_ok("set_network", id, "key_mgmt", "NONE");
	} else {
		quoted = g_strdup_printf("\"%s\"", pw);
		if (n->sae_only) {                          /* WPA3 only */
			wpa_ok("set_network", id, "key_mgmt", "SAE");
			wpa_ok("set_network", id, "ieee80211w", "2");
			wpa_ok("set_network", id, "sae_password", quoted);
		} else {                                    /* WPA2, or WPA2/WPA3 mixed */
			wpa_ok("set_network", id, "psk", quoted);
		}
		memset(quoted, 0, strlen(quoted));
		memset(pw, 0, strlen(pw));
		g_free(quoted);
		g_free(pw);
	}
	if (wpa_ok("select_network", id, NULL, NULL))
		wait_for_connection();
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
	gtk_window_set_default_size(GTK_WINDOW(win), 460, 480);
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
