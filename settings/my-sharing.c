/*
 * my-sharing: the Sharing window of my-distro (GTK 3), 0.9.7. Start menu
 * "Sharing", or Settings, Network.
 *
 * Shares one folder of your home (and, if you like, the printers) with the
 * other computers in your network: Windows, Macs and Linux open it as a
 * network share (SMB) and log in with your user name and a sharing password.
 * Off by default.
 *
 * Samba does the sharing. It is a package of the Software app (big, and
 * most people never need it): without it the window offers to install it.
 * The root part is /usr/lib/my-distro/sharing, run through my-admin (the
 * root password): it writes Samba's settings, sets the password, starts
 * the server and opens the firewall for it, only for local networks.
 */
#include <gtk/gtk.h>

#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define HELPER "/usr/lib/my-distro/sharing"

static GtkWidget *win, *stack, *folder_btn, *ro_radio, *rw_radio, *printers_check,
                 *pw1, *pw2, *status_label, *where_label, *btn_share, *btn_stop, *btn_install;
static char st_installed[8], st_on[8], st_running[8], st_user[64], st_folder[1024],
            st_readonly[8], st_printers[8], st_addresses[256];

static void read_status(void)
{
	const char *argv[] = { HELPER, "status", NULL };
	char *out = NULL, **lines;

	st_installed[0] = st_on[0] = st_running[0] = st_user[0] = st_folder[0] = '\0';
	st_readonly[0] = st_printers[0] = st_addresses[0] = '\0';
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
	                  &out, NULL, NULL, NULL) || !out)
		return;
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *eq = strstr(lines[i], " = ");
		const char *k = lines[i], *v;

		if (!eq)
			continue;
		*eq = '\0';
		v = eq + 3;
#define TAKE(name, dst) if (!strcmp(k, name)) g_strlcpy(dst, v, sizeof dst)
		TAKE("installed", st_installed);
		TAKE("on", st_on);
		TAKE("running", st_running);
		TAKE("user", st_user);
		TAKE("folder", st_folder);
		TAKE("readonly", st_readonly);
		TAKE("printers", st_printers);
		TAKE("addresses", st_addresses);
#undef TAKE
	}
	g_strfreev(lines);
	g_free(out);
}

/* "\\192.168.1.5\Public (Windows) ..." for the shared folder */
static void show_where(void)
{
	char *name, *t, addr[64] = "";

	if (strcmp(st_running, "yes") != 0) {
		gtk_label_set_text(GTK_LABEL(where_label), _("Sharing is off."));
		return;
	}
	sscanf(st_addresses, "%63s", addr);
	if (!addr[0])
		g_strlcpy(addr, g_get_host_name(), sizeof addr);
	/* the share's name as the helper makes it: A-Z a-z 0-9 _ - only, 24 at most */
	name = g_path_get_basename(st_folder);
	for (char *p = name; *p; p++)
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
			*p = '_';
	if (strlen(name) > 24)
		name[24] = '\0';
	t = g_strdup_printf(_("Shared. On other computers open \\\\%s\\%s (Windows) or "
	                      "smb://%s/%s (Mac, Linux) and log in as %s with the sharing password."),
	                    addr, name, addr, name, st_user);
	gtk_label_set_text(GTK_LABEL(where_label), t);
	g_free(t);
	g_free(name);
}

static void fill(void)
{
	read_status();
	if (strcmp(st_installed, "yes") != 0) {
		gtk_stack_set_visible_child_name(GTK_STACK(stack), "install");
		return;
	}
	gtk_stack_set_visible_child_name(GTK_STACK(stack), "settings");
	if (st_folder[0])
		gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(folder_btn), st_folder);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strcmp(st_readonly, "no") ? ro_radio : rw_radio), TRUE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(printers_check), !strcmp(st_printers, "yes"));
	gtk_entry_set_placeholder_text(GTK_ENTRY(pw1), st_user[0] ? _("(unchanged)") : "");
	gtk_widget_set_sensitive(btn_stop, !strcmp(st_running, "yes"));
	show_where();
}

/* Asks for the root password. NULL = cancelled. */
static char *ask_root_password(void)
{
	GtkWidget *d = gtk_dialog_new_with_buttons(_("Sharing"), GTK_WINDOW(win), GTK_DIALOG_MODAL,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("OK"), GTK_RESPONSE_OK, NULL);
	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	GtkWidget *l = gtk_label_new(_("Sharing changes the system: type the root password."));
	GtkWidget *e = gtk_entry_new();
	char *pw = NULL;

	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
		pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
	gtk_widget_destroy(d);
	return pw;
}

/* Runs my-admin ARGS with the root password (and more input lines after
 * it); shows the last "DONE:"/"ERROR:" line. Waits (it takes seconds,
 * installing Samba a minute). Returns my-admin's exit code. */
static int run_admin(const char **args, const char *more_input)
{
	char *pw = ask_root_password(), *input, *out = NULL;
	const char *argv[12] = { "/usr/bin/my-admin" };
	int n = 1, in, outfd, st = 1;
	GPid pid;
	GString *all = g_string_new(NULL);
	char buf[512];
	ssize_t r;

	if (!pw)
		return -1;
	for (int i = 0; args[i] && n < 11; i++)
		argv[n++] = args[i];
	argv[n] = NULL;
	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL,
	                              NULL, &pid, &in, &outfd, NULL, NULL)) {
		g_free(pw);
		return 1;
	}
	input = g_strdup_printf("%s\n%s", pw, more_input ? more_input : "");
	if (write(in, input, strlen(input)) < 0) {
		/* my-admin then says "wrong password" */
	}
	close(in);
	memset(input, 0, strlen(input));
	memset(pw, 0, strlen(pw));
	g_free(input);
	g_free(pw);
	gtk_label_set_text(GTK_LABEL(status_label), _("Working…"));
	/* read its output while keeping the window alive */
	while ((r = read(outfd, buf, sizeof buf)) != 0) {
		if (r > 0)
			g_string_append_len(all, buf, r);
		while (gtk_events_pending())
			gtk_main_iteration();
	}
	close(outfd);
	waitpid(pid, &st, 0);
	g_spawn_close_pid(pid);
	out = all->str;
	{
		char *e = strstr(out, "ERROR: ");

		if (WIFEXITED(st) && WEXITSTATUS(st) == 2)
			gtk_label_set_text(GTK_LABEL(status_label), _("Wrong password: nothing was done."));
		else if (e) {
			g_strdelimit(e, "\n", ' ');
			gtk_label_set_text(GTK_LABEL(status_label), e + 7);
		} else
			gtk_label_set_text(GTK_LABEL(status_label), "");
	}
	g_string_free(all, TRUE);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void on_install(GtkButton *b, gpointer d)
{
	const char *args[] = { "--pkg", "install", "samba4", NULL };

	(void)b; (void)d;
	gtk_widget_set_sensitive(btn_install, FALSE);
	run_admin(args, NULL);
	gtk_widget_set_sensitive(btn_install, TRUE);
	fill();
}

static void on_share(GtkButton *b, gpointer d)
{
	char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(folder_btn)), *pw;
	const char *a = gtk_entry_get_text(GTK_ENTRY(pw1)), *c = gtk_entry_get_text(GTK_ENTRY(pw2));
	const char *args[8];

	(void)b; (void)d;
	if (!folder) {
		gtk_label_set_text(GTK_LABEL(status_label), _("Choose a folder first."));
		return;
	}
	if (strcmp(a, c) != 0) {
		gtk_label_set_text(GTK_LABEL(status_label), _("The two passwords are not the same."));
		g_free(folder);
		return;
	}
	args[0] = "--sharing";
	args[1] = "on";
	args[2] = g_get_user_name();
	args[3] = folder;
	args[4] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(rw_radio)) ? "rw" : "ro";
	args[5] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(printers_check)) ? "printers" : "noprinters";
	args[6] = NULL;
	pw = g_strdup_printf("%s\n", a);
	run_admin(args, pw);
	memset(pw, 0, strlen(pw));
	g_free(pw);
	g_free(folder);
	gtk_entry_set_text(GTK_ENTRY(pw1), "");
	gtk_entry_set_text(GTK_ENTRY(pw2), "");
	fill();
}

static void on_stop(GtkButton *b, gpointer d)
{
	const char *args[] = { "--sharing", "off", NULL };

	(void)b; (void)d;
	run_admin(args, NULL);
	fill();
}

static GtkWidget *heading(const char *text)
{
	GtkWidget *l = gtk_label_new(NULL);
	char *m = g_markup_printf_escaped("<b>%s</b>", text);

	gtk_label_set_markup(GTK_LABEL(l), m);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	g_free(m);
	return l;
}

static GtkWidget *text(const char *t)
{
	GtkWidget *l = gtk_label_new(t);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
	return l;
}

int main(int argc, char **argv)
{
	GtkWidget *box, *inst, *set, *row, *grid;
	char *public;

	i18n_init();
	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), _("Sharing"));
	gtk_window_set_default_size(GTK_WINDOW(win), 520, -1);
	gtk_window_set_icon_name(GTK_WINDOW(win), "folder-publicshare");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(win), box);
	gtk_box_pack_start(GTK_BOX(box), text(_("Share a folder (and the printers) with the other "
		"computers in your network. They log in with your user name and a sharing password. "
		"Off until you switch it on.")), FALSE, FALSE, 0);
	stack = gtk_stack_new();
	gtk_stack_set_homogeneous(GTK_STACK(stack), FALSE);
	gtk_box_pack_start(GTK_BOX(box), stack, TRUE, TRUE, 0);

	/* Samba missing: offer to install it */
	inst = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_box_pack_start(GTK_BOX(inst), text(_("Sharing needs Samba, a package of the Software "
		"app (about 60 MB).")), FALSE, FALSE, 0);
	btn_install = gtk_button_new_with_label(_("Install Samba…"));
	gtk_widget_set_halign(btn_install, GTK_ALIGN_START);
	g_signal_connect(btn_install, "clicked", G_CALLBACK(on_install), NULL);
	gtk_box_pack_start(GTK_BOX(inst), btn_install, FALSE, FALSE, 0);
	gtk_stack_add_named(GTK_STACK(stack), inst, "install");

	/* The settings */
	set = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_box_pack_start(GTK_BOX(set), heading(_("Folder")), FALSE, FALSE, 0);
	folder_btn = gtk_file_chooser_button_new(_("Folder to share"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
	public = g_build_filename(g_get_home_dir(), "Public", NULL);
	g_mkdir_with_parents(public, 0755);
	gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(folder_btn), public);
	g_free(public);
	gtk_box_pack_start(GTK_BOX(set), folder_btn, FALSE, FALSE, 0);
	ro_radio = gtk_radio_button_new_with_label(NULL, _("Others may only read the files"));
	rw_radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(ro_radio),
		_("Others may also change and add files"));
	gtk_box_pack_start(GTK_BOX(set), ro_radio, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(set), rw_radio, FALSE, FALSE, 0);
	printers_check = gtk_check_button_new_with_label(_("Also share this computer's printers"));
	gtk_box_pack_start(GTK_BOX(set), printers_check, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(set), heading(_("Sharing password")), FALSE, FALSE, 6);
	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	pw1 = gtk_entry_new();
	pw2 = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(pw1), FALSE);
	gtk_entry_set_visibility(GTK_ENTRY(pw2), FALSE);
	gtk_widget_set_hexpand(pw1, TRUE);
	gtk_grid_attach(GTK_GRID(grid), text(_("Password")), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), pw1, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), text(_("Again")), 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), pw2, 1, 1, 1, 1);
	gtk_box_pack_start(GTK_BOX(set), grid, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(set), text(_("Not your login password: one only for sharing "
		"(at least 6 characters).")), FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	btn_share = gtk_button_new_with_label(_("Share"));
	gtk_style_context_add_class(gtk_widget_get_style_context(btn_share), "suggested-action");
	g_signal_connect(btn_share, "clicked", G_CALLBACK(on_share), NULL);
	btn_stop = gtk_button_new_with_label(_("Stop sharing"));
	g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop), NULL);
	gtk_box_pack_start(GTK_BOX(row), btn_share, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), btn_stop, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(set), row, FALSE, FALSE, 6);
	where_label = text("");
	gtk_label_set_selectable(GTK_LABEL(where_label), TRUE);
	gtk_box_pack_start(GTK_BOX(set), where_label, FALSE, FALSE, 0);
	gtk_stack_add_named(GTK_STACK(stack), set, "settings");

	status_label = text("");
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	fill();
	gtk_main();
	return 0;
}
