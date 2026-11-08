/*
 * my-share: opens a shared folder of a Windows PC or a NAS (SMB/CIFS) in
 * the file manager (GTK 3). Start menu, "Network share", or:
 *
 *     my-share [smb://SERVER/SHARE | \\SERVER\SHARE]
 *
 * You type the share's address, a user name and password (empty = as a
 * guest). "my-power mount-share" (setuid root, see login/my-power.c) mounts
 * it with mount.cifs at /media/SERVER-SHARE, so that only you can open it;
 * the password goes to it on its standard input. Then PCManFM shows it.
 * To disconnect: right-click the folder in /media, Eject (or my-eject).
 *
 * This PC doesn't share anything itself: the firewall lets no one in.
 */
#include <gtk/gtk.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "i18n.h"

static GtkWidget *win, *addr, *user, *pass, *status, *connect_btn;

/* "smb://srv/share/", "\\srv\share" or "srv/share" -> "//srv/share";
 * NULL if it doesn't name a server and a share. */
static char *unc_of(const char *in)
{
	char *s = g_strstrip(g_strdup(in)), *p = s, *out = NULL;

	g_strdelimit(s, "\\", '/');
	if (g_str_has_prefix(p, "smb://"))
		p += 6;
	while (*p == '/')
		p++;
	while (*p && p[strlen(p) - 1] == '/')
		p[strlen(p) - 1] = '\0';
	/* server/share, nothing deeper (a folder inside: open it in Files) */
	if (*p && strchr(p, '/') && strchr(p, '/')[1] && !strchr(strchr(p, '/') + 1, '/'))
		out = g_strdup_printf("//%s", p);
	g_free(s);
	return out;
}

static void say(const char *msg)
{
	gtk_label_set_text(GTK_LABEL(status), msg);
}

static void on_connect(GtkButton *b, gpointer d)
{
	char *unc = unc_of(gtk_entry_get_text(GTK_ENTRY(addr)));
	const char *name = gtk_entry_get_text(GTK_ENTRY(user));
	const char *argv[] = { "/usr/bin/my-power", "mount-share", unc, name[0] ? name : NULL, NULL };
	char *out = NULL, *err = NULL, *pw;
	GPid pid;
	int in, outfd, errfd, st;
	GError *e = NULL;

	(void)b; (void)d;
	if (!unc) {
		say(_("Write the address as \\\\SERVER\\SHARE or smb://SERVER/SHARE."));
		return;
	}
	say(_("Connecting…"));
	gtk_widget_set_sensitive(connect_btn, FALSE);
	while (gtk_events_pending())
		gtk_main_iteration();

	/* the password on standard input, then wait for the answer */
	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                              NULL, NULL, &pid, &in, &outfd, &errfd, &e)) {
		say(e->message);
		g_error_free(e);
		gtk_widget_set_sensitive(connect_btn, TRUE);
		g_free(unc);
		return;
	}
	pw = g_strdup_printf("%s\n", gtk_entry_get_text(GTK_ENTRY(pass)));
	if (write(in, pw, strlen(pw)) < 0) {
		/* my-power then reads an empty password */
	}
	close(in);
	memset(pw, 0, strlen(pw));
	g_free(pw);
	{
		GIOChannel *o = g_io_channel_unix_new(outfd), *er = g_io_channel_unix_new(errfd);

		g_io_channel_read_to_end(o, &out, NULL, NULL);
		g_io_channel_read_to_end(er, &err, NULL, NULL);
		g_io_channel_shutdown(o, FALSE, NULL);
		g_io_channel_shutdown(er, FALSE, NULL);
		g_io_channel_unref(o);
		g_io_channel_unref(er);
	}
	waitpid(pid, &st, 0);
	g_spawn_close_pid(pid);
	gtk_widget_set_sensitive(connect_btn, TRUE);

	if (g_spawn_check_wait_status(st, NULL) && out && out[0] == '/') {
		const char *files[] = { "pcmanfm", g_strstrip(out), NULL };

		g_spawn_async(NULL, (char **)files, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
		gtk_main_quit();
	} else {
		say(err && err[0] ? g_strstrip(err)
		    : _("The share could not be opened (wrong name or password, or the server "
		        "can't be reached)."));
	}
	g_free(out);
	g_free(err);
	g_free(unc);
}

static GtkWidget *row(GtkWidget *grid, int y, const char *caption, GtkWidget *w)
{
	GtkWidget *l = gtk_label_new(caption);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_hexpand(w, TRUE);
	gtk_grid_attach(GTK_GRID(grid), l, 0, y, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), w, 1, y, 1, 1);
	return w;
}

int main(int argc, char **argv)
{
	GtkWidget *box, *grid, *intro, *buttons, *cancel;

	i18n_init();
	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), _("Network share"));
	gtk_window_set_icon_name(GTK_WINDOW(win), "folder-remote");
	gtk_window_set_default_size(GTK_WINDOW(win), 460, -1);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_container_add(GTK_CONTAINER(win), box);
	intro = gtk_label_new(_("Open a shared folder of a Windows PC or a network drive (NAS). "
		"Leave the user name empty to connect as a guest."));
	gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(intro), 50);
	gtk_label_set_xalign(GTK_LABEL(intro), 0);
	gtk_box_pack_start(GTK_BOX(box), intro, FALSE, FALSE, 0);

	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	addr = row(grid, 0, _("Address"), gtk_entry_new());
	gtk_entry_set_placeholder_text(GTK_ENTRY(addr), "\\\\server\\share");
	user = row(grid, 1, _("User name"), gtk_entry_new());
	pass = row(grid, 2, _("Password"), gtk_entry_new());
	gtk_entry_set_visibility(GTK_ENTRY(pass), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(pass), TRUE);
	gtk_entry_set_activates_default(GTK_ENTRY(addr), TRUE);
	if (argc > 1)
		gtk_entry_set_text(GTK_ENTRY(addr), argv[1]);
	gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

	status = gtk_label_new("");
	gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
	gtk_label_set_xalign(GTK_LABEL(status), 0);
	gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);

	buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	cancel = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(cancel, "clicked", G_CALLBACK(gtk_main_quit), NULL);
	connect_btn = gtk_button_new_with_label(_("Connect"));
	gtk_style_context_add_class(gtk_widget_get_style_context(connect_btn), "suggested-action");
	gtk_widget_set_can_default(connect_btn, TRUE);
	g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect), NULL);
	gtk_box_pack_end(GTK_BOX(buttons), connect_btn, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(buttons), cancel, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), buttons, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	gtk_widget_grab_default(connect_btn);
	gtk_main();
	return 0;
}
