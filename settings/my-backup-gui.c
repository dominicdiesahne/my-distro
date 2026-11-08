/*
 * my-backup-gui: the Backup window of my-distro (GTK 3), Start menu "Backup".
 *
 * Choose a USB stick or disk (the ones open in /media), click "Back up now":
 * my-backup (a shell script with rsync) copies your home folder there. The
 * list shows the backups on that disk; "Restore" puts one back.
 *
 * Your own files need no password. With "Also the system settings" the
 * window asks for the root password and runs my-backup through my-admin
 * (my-admin --backup ...), because those files belong to root.
 *
 * my-backup prints PROGRESS: (percent), DONE: and ERROR: lines; the window
 * shows them. my-backup sends the notification when it is done.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

static GtkWidget *win, *disk_combo, *disk_info, *settings_check, *list, *progress,
                 *status_label, *btn_backup, *btn_restore;
static int running;

/* --- The disks: everything open in /media -------------------------------------------- */

static void fill_disks(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	char dev[256], dir[512], type[64], *old = NULL;
	int n = 0;

	if (gtk_combo_box_get_active_id(GTK_COMBO_BOX(disk_combo)))
		old = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(disk_combo)));
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(disk_combo));
	while (f && fscanf(f, "%255s %511s %63s %*[^\n]", dev, dir, type) == 3) {
		char *path, *name;

		if (strncmp(dir, "/media/", 7) != 0 || !strcmp(type, "iso9660") || !strcmp(type, "udf"))
			continue;
		path = g_strcompress(dir);        /* /proc/mounts writes a space as \040 */
		name = g_path_get_basename(path);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(disk_combo), path, name);
		g_free(name);
		g_free(path);
		n++;
	}
	if (f)
		fclose(f);
	if (!old || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(disk_combo), old))
		gtk_combo_box_set_active(GTK_COMBO_BOX(disk_combo), 0);
	g_free(old);
	if (!n)
		gtk_label_set_text(GTK_LABEL(disk_info), "Plug in a USB stick or disk: it opens "
		                   "by itself and appears here.");
}

static const char *chosen_disk(void)
{
	return gtk_combo_box_get_active_id(GTK_COMBO_BOX(disk_combo));
}

/* --- The backups on the chosen disk ---------------------------------------------------- */

static void fill_list(void)
{
	const char *disk = chosen_disk();
	const char *argv[] = { "my-backup", "list", disk, NULL };
	GList *old = gtk_container_get_children(GTK_CONTAINER(list));
	char *out = NULL, **lines;
	struct statvfs st;
	char fstype[64] = "";

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
	gtk_widget_set_sensitive(btn_backup, disk && !running);
	gtk_widget_set_sensitive(btn_restore, FALSE);
	if (!disk)
		return;

	/* free room, and a word about FAT */
	{
		FILE *f = fopen("/proc/mounts", "r");
		char d[256], dir[512], type[64];

		while (f && fscanf(f, "%255s %511s %63s %*[^\n]", d, dir, type) == 3) {
			char *u = g_strcompress(dir);   /* \040 -> space */

			if (!strcmp(u, disk))
				g_strlcpy(fstype, type, sizeof fstype);
			g_free(u);
		}
		if (f)
			fclose(f);
	}
	if (statvfs(disk, &st) == 0) {
		char *free_s = g_format_size((guint64)st.f_bavail * st.f_frsize);
		char *t = g_strdup_printf("%s free (%s).%s", free_s, fstype,
			!strcmp(fstype, "vfat") || !strcmp(fstype, "exfat") ?
			" On FAT and exFAT every backup is a full copy; formatted as ext4 "
			"(Disks) only the changes take room." : "");

		gtk_label_set_text(GTK_LABEL(disk_info), t);
		g_free(t);
		g_free(free_s);
	}

	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, NULL, NULL) || !out)
		return;
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char name[64], date[32], time_s[16], *t;
		GtkWidget *l;
		int y, mo, d, h, mi, s;

		if (sscanf(lines[i], "%63s", name) != 1 ||
		    sscanf(name, "%4d-%2d-%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6)
			continue;
		snprintf(date, sizeof date, "%02d.%02d.%04d", d, mo, y);
		snprintf(time_s, sizeof time_s, "%02d:%02d", h, mi);
		t = g_strdup_printf("%s  %s%s", date, time_s,
		                    strstr(lines[i], "settings") ? "   (with system settings)" : "");
		l = gtk_label_new(t);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		g_object_set(l, "margin", 8, NULL);
		g_object_set_data_full(G_OBJECT(l), "name", g_strdup(name), g_free);
		g_object_set_data(G_OBJECT(l), "settings", GINT_TO_POINTER(strstr(lines[i], "settings") != NULL));
		gtk_list_box_insert(GTK_LIST_BOX(list), l, -1);
		g_free(t);
	}
	g_strfreev(lines);
	g_free(out);
	gtk_widget_show_all(list);
}

static void on_disk(GtkComboBox *c, gpointer d)
{
	(void)c; (void)d;
	fill_list();
}

static void on_row(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)d;
	gtk_widget_set_sensitive(btn_restore, r && !running);
}

/* --- Running my-backup -------------------------------------------------------------- */

static gboolean on_output(GIOChannel *ch, GIOCondition c, gpointer d)
{
	char *line = NULL;
	gsize len;

	(void)c; (void)d;
	if (g_io_channel_read_line(ch, &line, &len, NULL, NULL) != G_IO_STATUS_NORMAL) {
		g_free(line);
		return G_SOURCE_REMOVE;
	}
	g_strchomp(line);
	if (g_str_has_prefix(line, "PROGRESS: "))
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), atoi(line + 10) / 100.0);
	else if (g_str_has_prefix(line, "DONE: ") || g_str_has_prefix(line, "ERROR: "))
		gtk_label_set_text(GTK_LABEL(status_label), line + (line[0] == 'D' ? 6 : 7));
	else if (line[0] && !g_str_has_prefix(line, "Note"))
		gtk_label_set_text(GTK_LABEL(status_label), line);
	g_free(line);
	return G_SOURCE_CONTINUE;
}

static void on_done(GPid pid, gint st, gpointer d)
{
	(void)d;
	g_spawn_close_pid(pid);
	running = 0;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 2)        /* my-admin: wrong password */
		gtk_label_set_text(GTK_LABEL(status_label), "Wrong password: nothing was done.");
	else if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 1.0);
	gtk_widget_set_sensitive(disk_combo, TRUE);
	fill_list();
}

/* Asks for the root password (for the system settings). NULL = cancelled. */
static char *ask_root_password(void)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Backup", GTK_WINDOW(win), GTK_DIALOG_MODAL,
		"Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);
	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	GtkWidget *l = gtk_label_new("The system settings belong to the administrator: "
	                             "type the root password.");
	GtkWidget *e = gtk_entry_new();
	char *pw = NULL;

	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 44);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
		pw = g_strdup_printf("%s\n", gtk_entry_get_text(GTK_ENTRY(e)));
	gtk_widget_destroy(d);
	return pw;
}

/* Runs my-backup ACTION DISK [NAME] [--settings]; with the settings through
 * my-admin, as root. */
static void start(const char *action, const char *name, int with_settings)
{
	const char *argv[10];
	char *pw = NULL;
	int n = 0, in = -1, out;
	GPid pid;
	GIOChannel *ch;

	if (with_settings) {
		if (!(pw = ask_root_password()))
			return;
		argv[n++] = "/usr/bin/my-admin";
		argv[n++] = "--backup";
	} else {
		argv[n++] = "my-backup";
	}
	argv[n++] = action;
	argv[n++] = chosen_disk();
	if (name)
		argv[n++] = name;
	if (with_settings) {
		argv[n++] = "--settings";
		argv[n++] = "--user";
		argv[n++] = g_get_user_name();
	}
	argv[n] = NULL;

	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL,
	                              G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, NULL, NULL,
	                              &pid, pw ? &in : NULL, &out, NULL, NULL)) {
		gtk_label_set_text(GTK_LABEL(status_label), "Could not start my-backup.");
		g_free(pw);
		return;
	}
	if (pw) {
		if (write(in, pw, strlen(pw)) < 0) {
			/* my-admin then says "wrong password" */
		}
		close(in);
		memset(pw, 0, strlen(pw));
		g_free(pw);
	}
	running = 1;
	gtk_widget_set_sensitive(btn_backup, FALSE);
	gtk_widget_set_sensitive(btn_restore, FALSE);
	gtk_widget_set_sensitive(disk_combo, FALSE);
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0);
	gtk_label_set_text(GTK_LABEL(status_label), "Starting…");
	ch = g_io_channel_unix_new(out);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_add_watch(ch, G_IO_IN | G_IO_HUP, on_output, NULL);
	g_io_channel_unref(ch);
	g_child_watch_add(pid, on_done, NULL);
}

static void on_backup(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	if (chosen_disk())
		start("backup", NULL, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(settings_check)));
}

static void on_restore(GtkButton *b, gpointer d)
{
	GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	GtkWidget *row, *dlg, *box, *also = NULL;
	const char *name;
	int has_settings, ok, with = 0;

	(void)b; (void)d;
	if (!r)
		return;
	row = gtk_bin_get_child(GTK_BIN(r));
	name = g_object_get_data(G_OBJECT(row), "name");
	has_settings = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "settings"));
	dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
		GTK_BUTTONS_OK_CANCEL, "Put this backup back?");
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
		"Its files replace the ones with the same name in your home folder. Files you "
		"made after the backup stay. (File history keeps the versions from before: "
		"my-undo can bring them back.)");
	if (has_settings) {
		box = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dlg));
		also = gtk_check_button_new_with_label("Also the system settings (asks for the root password)");
		gtk_box_pack_start(GTK_BOX(box), also, FALSE, FALSE, 0);
		gtk_widget_show(also);
	}
	ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
	if (also)
		with = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(also));
	gtk_widget_destroy(dlg);
	if (ok)
		start("restore", name, with);
}

static void on_refresh(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	fill_disks();
	fill_list();
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

int main(int argc, char **argv)
{
	GtkWidget *box, *row, *scroll, *refresh, *hint;

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Backup");
	gtk_window_set_default_size(GTK_WINDOW(win), 560, 520);
	gtk_window_set_icon_name(GTK_WINDOW(win), "document-save");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(win), box);

	gtk_box_pack_start(GTK_BOX(box), heading("Back up to"), FALSE, FALSE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	disk_combo = gtk_combo_box_text_new();
	g_signal_connect(disk_combo, "changed", G_CALLBACK(on_disk), NULL);
	refresh = gtk_button_new_with_label("Refresh");
	g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), NULL);
	gtk_box_pack_start(GTK_BOX(row), disk_combo, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), refresh, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	disk_info = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(disk_info), 0);
	gtk_label_set_line_wrap(GTK_LABEL(disk_info), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(disk_info), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), disk_info, FALSE, FALSE, 0);

	settings_check = gtk_check_button_new_with_label(
		"Also the system settings: Wi-Fi, VPNs, printers, Bluetooth, users "
		"(asks for the root password)");
	gtk_box_pack_start(GTK_BOX(box), settings_check, FALSE, FALSE, 0);
	hint = gtk_label_new("Your home folder is always backed up (without caches and the "
	                     "trash). The settings include saved passwords: keep the disk safe.");
	gtk_label_set_xalign(GTK_LABEL(hint), 0);
	gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

	btn_backup = gtk_button_new_with_label("Back up now");
	gtk_style_context_add_class(gtk_widget_get_style_context(btn_backup), "suggested-action");
	g_signal_connect(btn_backup, "clicked", G_CALLBACK(on_backup), NULL);
	gtk_widget_set_halign(btn_backup, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(box), btn_backup, FALSE, FALSE, 0);

	progress = gtk_progress_bar_new();
	gtk_box_pack_start(GTK_BOX(box), progress, FALSE, FALSE, 0);
	status_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status_label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
	gtk_label_set_ellipsize(GTK_LABEL(status_label), PANGO_ELLIPSIZE_END);
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(box), heading("Backups on this disk"), FALSE, FALSE, 0);
	list = gtk_list_box_new();
	g_signal_connect(list, "row-selected", G_CALLBACK(on_row), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
	btn_restore = gtk_button_new_with_label("Restore…");
	g_signal_connect(btn_restore, "clicked", G_CALLBACK(on_restore), NULL);
	gtk_widget_set_halign(btn_restore, GTK_ALIGN_END);
	gtk_box_pack_start(GTK_BOX(box), btn_restore, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	fill_disks();
	fill_list();
	gtk_main();
	return 0;
}
