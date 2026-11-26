/*
 * my-share: opens a shared folder of a Windows PC or a NAS (SMB/CIFS) in
 * the file manager (GTK 3). Start menu, "Network share", or:
 *
 *     my-share [smb://[USER@]SERVER/SHARE[/FOLDER] | \\SERVER\SHARE[\FOLDER]]
 *
 * 1.3.0: smb:// addresses open here from anywhere: links (xdg-open, this is
 * the handler of x-scheme-handler/smb) and the file manager's address bar
 * (a patch to PCManFM, desktop/patches/pcmanfm). A folder inside the share
 * opens after the share is mounted; a share that is mounted already opens
 * at once, without this window.
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

/* "smb://[user@]srv/share/dir/", "\\srv\share\dir" or "srv/share" ->
 * "//srv/share", with the folder inside the share ("dir", or "") in *rest
 * and the user name of the address (or NULL) in *user, if those are given
 * (g_free them). NULL if it doesn't name a server and a share. */
static char *unc_of(const char *in, char **rest, char **user)
{
	char *s = g_strstrip(g_strdup(in)), *p = s, *out = NULL, *slash, *at, *dir = NULL;

	if (rest)
		*rest = NULL;
	if (user)
		*user = NULL;
	g_strdelimit(s, "\\", '/');
	if (g_ascii_strncasecmp(p, "smb://", 6) == 0) {
		char *u;

		p += 6;
		/* %20 and the like in links */
		if ((u = g_uri_unescape_string(p, NULL))) {
			memmove(s, u, strlen(u) + 1);
			p = s;
			g_free(u);
		}
		/* user@server */
		slash = strchr(p, '/');
		if ((at = strchr(p, '@')) && (!slash || at < slash)) {
			if (user)
				*user = g_strndup(p, at - p);
			p = at + 1;
		}
	}
	while (*p == '/')
		p++;
	while (*p && p[strlen(p) - 1] == '/')
		p[strlen(p) - 1] = '\0';
	if (*p && (slash = strchr(p, '/')) && slash[1]) {
		char *deeper = strchr(slash + 1, '/');

		if (deeper) {
			dir = g_strdup(deeper + 1);
			*deeper = '\0';
		}
		out = g_strdup_printf("//%s", p);
	}
	if (rest && out)
		*rest = dir ? dir : g_strdup("");
	else
		g_free(dir);
	if (!out && user) {
		g_free(*user);
		*user = NULL;
	}
	g_free(s);
	return out;
}

/* Where my-power mounts it: /media/SERVER-SHARE (spaces and $ as _) */
static char *mount_dir(const char *unc)
{
	char *d = g_strdup_printf("/media/%s", unc + 2), *c;

	for (c = d + 7; *c; c++)
		if (*c == '/')
			*c = '-';
		else if (*c == ' ' || *c == '$')
			*c = '_';
	return d;
}

static int is_mounted(const char *dir)
{
	char *text = NULL, **lines;
	int found = 0;

	if (!g_file_get_contents("/proc/mounts", &text, NULL, NULL))
		return 0;
	lines = g_strsplit(text, "\n", -1);
	for (int i = 0; lines[i] && !found; i++) {
		char **f = g_strsplit(lines[i], " ", 3);

		found = f[0] && f[1] && strcmp(f[1], dir) == 0;
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(text);
	return found;
}

/* The file manager in DIR, or in the folder REST inside it if that exists */
static void open_folder(const char *dir, const char *rest)
{
	char *path = rest && *rest ? g_build_filename(dir, rest, NULL) : g_strdup(dir);
	const char *files[] = { "pcmanfm", NULL, NULL };

	if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
		g_free(path);
		path = g_strdup(dir);
	}
	files[1] = path;
	g_spawn_async(NULL, (char **)files, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
	g_free(path);
}

static void say(const char *msg)
{
	gtk_label_set_text(GTK_LABEL(status), msg);
}

static void on_connect(GtkButton *b, gpointer d)
{
	char *rest = NULL, *unc = unc_of(gtk_entry_get_text(GTK_ENTRY(addr)), &rest, NULL);
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
		open_folder(g_strstrip(out), rest);
		gtk_main_quit();
	} else {
		say(err && err[0] ? g_strstrip(err)
		    : _("The share could not be opened (wrong name or password, or the server "
		        "can't be reached)."));
	}
	g_free(out);
	g_free(err);
	g_free(unc);
	g_free(rest);
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
	char *url_user = NULL;

	i18n_init();
	gtk_init(&argc, &argv);
	/* a share that is mounted already: open it, no questions */
	if (argc > 1) {
		char *rest = NULL, *unc = unc_of(argv[1], &rest, &url_user);

		if (unc) {
			char *dir = mount_dir(unc);
			int done = is_mounted(dir);

			if (done)
				open_folder(dir, rest);
			g_free(dir);
			g_free(unc);
			g_free(rest);
			if (done)
				return 0;
		}
	}
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
	if (argc > 1)                      /* the address is there: the login next */
		gtk_widget_grab_focus(url_user ? pass : user);
	if (url_user) {
		gtk_entry_set_text(GTK_ENTRY(user), url_user);
		g_free(url_user);
	}
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
