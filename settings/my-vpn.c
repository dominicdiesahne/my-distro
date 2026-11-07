/*
 * my-vpn: the VPN window of my-distro (GTK 3), in the Start menu.
 *
 * VPN services and routers give you a WireGuard config file (NAME.conf, the
 * same file the official WireGuard apps import). "Import…" saves it; then
 * Connect / Disconnect. Only one VPN is connected at a time.
 *
 * The work that needs root is done by my-vpnctl (setuid, for members of the
 * group netdev like the Wi-Fi window), which runs /usr/lib/my-distro/vpn.
 */
#include <ctype.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CTL "/usr/bin/my-vpnctl"

static GtkWidget *win, *status_label, *detail_label, *list, *btn_connect, *btn_disconnect,
                 *btn_remove;
static char connected[32];        /* the connected VPN's name, or "" */
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
		if (write(in, input, strlen(input)) < 0) {
			/* my-vpnctl then says the file is empty */
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

		if (!names[i][0])
			continue;
		row = gtk_list_box_row_new();
		box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		name = gtk_label_new(names[i]);
		info = gtk_label_new(strcmp(names[i], connected) == 0 ? "connected" : "");
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
		text = g_strdup_printf("Connected to %s", connected);
		/* WireGuard talks to the server at least every 2 minutes while
		 * something is sent; no answer at all means a wrong config or a
		 * blocked network */
		if (secs < 0 && g_get_monotonic_time() - connected_since > 15 * G_USEC_PER_SEC)
			detail = g_strdup_printf("Address %s\nNo answer from the VPN server yet. Is the "
			                         "config right, and does this network allow VPNs?", addr);
		else if (secs < 0)
			detail = g_strdup_printf("Address %s\nWaiting for the VPN server…", addr);
		else
			detail = g_strdup_printf("Address %s\nLast answer from the server %d s ago · "
			                         "received %s · sent %s", addr, secs, rx, tx);
	} else {
		text = g_strdup("Not connected.");
		detail = g_strdup("Import the WireGuard config file (.conf) from your VPN service "
		                  "or router, then connect.");
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
	if (!g_spawn_check_wait_status(st, NULL) && strcmp(action, "up") == 0)
		gtk_label_set_text(GTK_LABEL(status_label), "Could not connect.");
	refresh_status(NULL);
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
	                             GTK_BUTTONS_NONE, "Remove the VPN \"%s\"?", n);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(ask),
		"To use it again you need its config file.");
	gtk_dialog_add_buttons(GTK_DIALOG(ask), "Cancel", GTK_RESPONSE_CANCEL,
	                       "Remove", GTK_RESPONSE_ACCEPT, NULL);
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

	for (int i = 0; names[i]; i++)
		found |= strcmp(names[i], name) == 0;
	g_strfreev(names);
	g_free(out);
	return found;
}

static void on_import(GtkButton *b, gpointer d)
{
	GtkWidget *fc;
	GtkFileFilter *f = gtk_file_filter_new();
	char *path = NULL, *content = NULL, *out, name[32];
	gsize len;
	int ok;

	(void)b; (void)d;
	fc = gtk_file_chooser_dialog_new("Import a WireGuard config", GTK_WINDOW(win),
		GTK_FILE_CHOOSER_ACTION_OPEN, "Cancel", GTK_RESPONSE_CANCEL,
		"Import", GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_filter_set_name(f, "WireGuard configs (*.conf)");
	gtk_file_filter_add_pattern(f, "*.conf");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), f);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), g_get_home_dir());
	if (gtk_dialog_run(GTK_DIALOG(fc)) == GTK_RESPONSE_ACCEPT)
		path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
	gtk_widget_destroy(fc);
	if (!path)
		return;
	if (!g_file_get_contents(path, &content, &len, NULL) || len > 60000) {
		message(GTK_MESSAGE_ERROR, "Could not read that file.", NULL);
		g_free(path);
		g_free(content);
		return;
	}
	name_from_file(path, name, sizeof name);
	if (exists(name)) {
		GtkWidget *ask = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
			GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "Replace the VPN \"%s\"?", name);
		int answer;

		gtk_dialog_add_buttons(GTK_DIALOG(ask), "Cancel", GTK_RESPONSE_CANCEL,
		                       "Replace", GTK_RESPONSE_ACCEPT, NULL);
		answer = gtk_dialog_run(GTK_DIALOG(ask));
		gtk_widget_destroy(ask);
		if (answer != GTK_RESPONSE_ACCEPT) {
			memset(content, 0, len);
			g_free(content);
			g_free(path);
			return;
		}
	}
	out = ctl("import", name, content, &ok);
	memset(content, 0, len);          /* the private key */
	g_free(content);
	if (ok) {
		fill_list(name);
		if (out && out[0])                 /* what was left out */
			message(GTK_MESSAGE_INFO, "VPN imported.", out);
	} else {
		message(GTK_MESSAGE_ERROR, "Could not import this file.", out);
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

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "VPN");
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
	btn_connect = button("Connect", G_CALLBACK(on_connect));
	gtk_style_context_add_class(gtk_widget_get_style_context(btn_connect), "suggested-action");
	btn_disconnect = button("Disconnect", G_CALLBACK(on_disconnect));
	btn_remove = button("Remove…", G_CALLBACK(on_remove));
	gtk_box_pack_start(GTK_BOX(row), button("Import…", G_CALLBACK(on_import)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), btn_remove, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_connect, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_disconnect, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	/* Not allowed (not in netdev): say so instead of an empty window */
	out = ctl("list", NULL, NULL, &ok);
	if (!ok) {
		gtk_label_set_text(GTK_LABEL(status_label), "VPN not available");
		gtk_label_set_text(GTK_LABEL(detail_label), out && out[0] ? out :
			"my-vpnctl is missing.");
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
