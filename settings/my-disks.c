/*
 * my-disks: the Disks window of my-distro (GTK 3), Start menu "Disks".
 *
 * Lists the disks and their partitions (lsblk), and can
 *   - format a USB stick, an SD card or a partition: FAT32, exFAT, ext4, NTFS
 *   - check and repair a filesystem (fsck)
 * The work is done by /usr/lib/my-distro/disk-tool (a shell script); this
 * window shows what it prints.
 *
 * Changing disks needs root. Like "Users and system": started as a normal
 * user, it asks for the root password and starts my-admin --disks, which
 * checks it and starts `my-disks --manage` as root.
 *
 * Safety: the partitions the running system uses (/, /boot, swap) can't be
 * chosen at all (disk-tool refuses them too). Other partitions on the same
 * disk, and every internal disk (a Windows disk, say), need the device's
 * name typed in after a big red warning. USB sticks and SD cards only need
 * a click on "Format".
 */
#include <gtk/gtk.h>

#include "i18n.h"
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TOOL "/usr/lib/my-distro/disk-tool"

enum { COL_ICON, COL_NAME, COL_SIZE, COL_FS, COL_WHERE, COL_PATH, COL_KIND, N_COLS };

/* kind of a row: what may be done with it */
enum { K_DISK, K_PART, K_SYSTEM_USED, K_SYSTEM_DISK };

struct blk {
	char name[32], path[64], type[16], fstype[24], label[64], mount[256];
	char tran[16], model[64], pkname[32];
	long long size;
	int rm, ro;
};

static struct blk blks[128];
static int nblks;
static GtkWidget *win, *tree, *output, *btn_format, *btn_check, *status_label;
static GtkTreeStore *store;
static int running;

/* --- Reading the disks (lsblk) ---------------------------------------------------------- */

/* lsblk -P writes NAME="sda" SIZE="8589934592" ... with odd characters as \xNN */
static void unescape(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (s[0] == '\\' && s[1] == 'x' && g_ascii_isxdigit(s[2]) && g_ascii_isxdigit(s[3])) {
			*o++ = (char)(g_ascii_xdigit_value(s[2]) * 16 + g_ascii_xdigit_value(s[3]));
			s += 3;
		} else {
			*o++ = *s;
		}
	}
	*o = '\0';
}

static void field(struct blk *b, const char *key, char *val)
{
	unescape(val);
#define F(k, f) else if (!strcmp(key, k)) g_strlcpy(b->f, val, sizeof b->f)
	if (!strcmp(key, "SIZE"))
		b->size = atoll(val);
	else if (!strcmp(key, "RM"))
		b->rm = atoi(val);
	else if (!strcmp(key, "RO"))
		b->ro = atoi(val);
	F("NAME", name); F("PATH", path); F("TYPE", type); F("FSTYPE", fstype);
	F("LABEL", label); F("MOUNTPOINT", mount); F("TRAN", tran); F("MODEL", model);
	F("PKNAME", pkname);
#undef F
}

static void read_disks(void)
{
	char *out = NULL, **lines;
	/* util-linux's lsblk: BusyBox's (/usr/bin) knows none of these options */
	const char *argv[] = { "/bin/lsblk", "-P", "-b", "-o",
		"NAME,PATH,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINT,TRAN,MODEL,PKNAME,RM,RO", NULL };

	nblks = 0;
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, NULL, NULL) || !out)
		return;
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i] && nblks < (int)G_N_ELEMENTS(blks); i++) {
		struct blk *b = &blks[nblks];
		char *p = lines[i];

		memset(b, 0, sizeof *b);
		/* KEY="value" KEY="value" ... */
		while (*p) {
			char *eq = strstr(p, "=\""), *end;

			if (!eq || !(end = strchr(eq + 2, '"')))
				break;
			*eq = *end = '\0';
			field(b, g_strstrip(p), eq + 2);
			p = end + 1;
		}
		/* disks and partitions; no RAM disks, CDs, zram swap, loop files */
		if ((!strcmp(b->type, "disk") || !strcmp(b->type, "part")) &&
		    strncmp(b->name, "zram", 4) && strncmp(b->name, "ram", 3) && b->size > 0)
			nblks++;
	}
	g_strfreev(lines);
	g_free(out);
}

/* Used by the running system: mounted outside /media, or swap */
static int system_used(const struct blk *b)
{
	return (b->mount[0] && strncmp(b->mount, "/media/", 7) != 0) ||
	       !strcmp(b->fstype, "swap") || !strcmp(b->label, "MY_DISTRO");
}

/* A disk that has a partition the running system uses (or the boot medium) */
static int system_disk(const struct blk *disk)
{
	if (system_used(disk))
		return 1;
	for (int i = 0; i < nblks; i++)
		if (!strcmp(blks[i].pkname, disk->name) && system_used(&blks[i]))
			return 1;
	return 0;
}

static int removable(const struct blk *disk)
{
	return !strcmp(disk->tran, "usb") || !strncmp(disk->name, "mmcblk", 6) || disk->rm;
}

static char *size_text(long long bytes)
{
	return g_format_size_full(bytes, G_FORMAT_SIZE_DEFAULT);
}

static void fill_tree(void)
{
	GtkTreeIter it, child;

	read_disks();
	gtk_tree_store_clear(store);
	for (int i = 0; i < nblks; i++) {
		struct blk *d = &blks[i];
		int sys, kind;
		char *size, *name;

		if (strcmp(d->type, "disk") != 0)
			continue;
		sys = system_disk(d);
		size = size_text(d->size);
		name = g_strdup_printf("%s%s(%s)%s", d->model[0] ? d->model : _("Disk"),
		                       d->model[0] ? " " : " ", d->name,
		                       sys ? _("  \xe2\x80\x94 my-distro runs from it") :
		                       removable(d) ? _("  \xe2\x80\x94 USB / card") : _("  \xe2\x80\x94 internal"));
		kind = sys ? K_SYSTEM_DISK : K_DISK;
		gtk_tree_store_append(store, &it, NULL);
		gtk_tree_store_set(store, &it, COL_ICON, removable(d) ? "drive-removable-media" : "drive-harddisk",
			COL_NAME, name, COL_SIZE, size, COL_FS, d->fstype, COL_WHERE, d->mount,
			COL_PATH, d->path, COL_KIND, kind, -1);
		g_free(size);
		g_free(name);
		for (int j = 0; j < nblks; j++) {
			struct blk *p = &blks[j];

			if (strcmp(p->type, "part") != 0 || strcmp(p->pkname, d->name) != 0)
				continue;
			size = size_text(p->size);
			name = g_strdup_printf("%s%s%s", p->name, p->label[0] ? "  " : "", p->label);
			gtk_tree_store_append(store, &child, &it);
			gtk_tree_store_set(store, &child, COL_ICON, "drive-harddisk",
				COL_NAME, name, COL_SIZE, size, COL_FS, p->fstype,
				COL_WHERE, system_used(p) && !p->mount[0] ? _("(used by the system)") : p->mount,
				COL_PATH, p->path, COL_KIND, system_used(p) ? K_SYSTEM_USED :
				sys ? K_SYSTEM_DISK : K_PART, -1);
			g_free(size);
			g_free(name);
		}
	}
	gtk_tree_view_expand_all(GTK_TREE_VIEW(tree));
}

/* --- The chosen row ---------------------------------------------------------------- */

static int chosen(char *path, size_t len, int *kind, char *fs, size_t fslen, int *is_disk)
{
	GtkTreeModel *m;
	GtkTreeIter it;
	char *p, *f;

	if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &m, &it))
		return 0;
	gtk_tree_model_get(m, &it, COL_PATH, &p, COL_KIND, kind, COL_FS, &f, -1);
	g_strlcpy(path, p, len);
	g_strlcpy(fs, f ? f : "", fslen);
	*is_disk = gtk_tree_store_iter_depth(store, &it) == 0;
	g_free(p);
	g_free(f);
	return 1;
}

static void update_buttons(void)
{
	char path[64], fs[24];
	int kind = 0, disk = 0, ok = chosen(path, sizeof path, &kind, fs, sizeof fs, &disk);

	/* a whole disk with the system on it: no (its other partitions: yes) */
	gtk_widget_set_sensitive(btn_format, ok && !running && kind != K_SYSTEM_USED &&
	                         !(disk && kind == K_SYSTEM_DISK));
	gtk_widget_set_sensitive(btn_check, ok && !running && kind != K_SYSTEM_USED && fs[0]);
}

static void on_select(GtkTreeSelection *s, gpointer d)
{
	(void)s; (void)d;
	update_buttons();
}

/* --- Running disk-tool -------------------------------------------------------------- */

static void append(const char *text)
{
	GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(output));
	GtkTextIter end;

	gtk_text_buffer_get_end_iter(b, &end);
	gtk_text_buffer_insert(b, &end, text, -1);
	gtk_text_buffer_get_end_iter(b, &end);
	gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(output), &end, 0, FALSE, 0, 0);
}

static gboolean on_output(GIOChannel *ch, GIOCondition c, gpointer d)
{
	char *line = NULL;
	gsize len;

	(void)c; (void)d;
	if (g_io_channel_read_line(ch, &line, &len, NULL, NULL) != G_IO_STATUS_NORMAL) {
		g_free(line);
		return G_SOURCE_REMOVE;
	}
	append(line);
	if (g_str_has_prefix(line, "DONE: ") || g_str_has_prefix(line, "ERROR: ")) {
		g_strchomp(line);
		gtk_label_set_text(GTK_LABEL(status_label), line + (line[0] == 'D' ? 6 : 7));
	}
	g_free(line);
	return G_SOURCE_CONTINUE;
}

static void on_done(GPid pid, gint st, gpointer d)
{
	(void)st; (void)d;
	g_spawn_close_pid(pid);
	running = 0;
	fill_tree();
	update_buttons();
}

static void start_tool(const char *const *argv)
{
	GPid pid;
	int out;
	GIOChannel *ch;

	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(output)), "", -1);
	gtk_label_set_text(GTK_LABEL(status_label), _("Working… (don't pull out the disk)"));
	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                              NULL, NULL, &pid, NULL, &out, NULL, NULL)) {
		gtk_label_set_text(GTK_LABEL(status_label), _("Could not start /usr/lib/my-distro/disk-tool."));
		return;
	}
	running = 1;
	update_buttons();
	ch = g_io_channel_unix_new(out);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_add_watch(ch, G_IO_IN | G_IO_HUP, on_output, NULL);
	g_io_channel_unref(ch);
	g_child_watch_add(pid, on_done, NULL);
}

/* The big warning for the system's disk and internal disks: type the
 * device's name to go on. Returns 1 if typed right. */
static int dangerous_ok(const char *path, int kind)
{
	const char *name = strrchr(path, '/') + 1;
	GtkWidget *d = gtk_dialog_new_with_buttons(_("Careful"), GTK_WINDOW(win), GTK_DIALOG_MODAL,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("Delete everything"), GTK_RESPONSE_OK, NULL);
	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d)), *l = gtk_label_new(NULL);
	GtkWidget *e = gtk_entry_new();
	char *m = g_markup_printf_escaped(
		_("<span size='x-large' weight='bold' foreground='#e5484d'>%s</span>\n\n"
		"Everything on <b>%s</b> will be deleted: documents, photos, other systems "
		"(Windows) that may be on it. This can't be undone.\n\n"
		"To go on, type its name: <b>%s</b>"),
		kind == K_SYSTEM_DISK ? _("This is the disk my-distro runs from!") :
		                        _("This is a disk inside the computer!"), path, name);
	int ok = 0;

	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 12);
	gtk_label_set_markup(GTK_LABEL(l), m);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(
		gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_OK)), "destructive-action");
	gtk_widget_show_all(d);
	while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
		if (!strcmp(gtk_entry_get_text(GTK_ENTRY(e)), name)) {
			ok = 1;
			break;
		}
	}
	gtk_widget_destroy(d);
	g_free(m);
	return ok;
}

/* Is the disk of this device (or the device itself) a USB stick / card? */
static int on_removable(const char *path)
{
	for (int i = 0; i < nblks; i++)
		if (!strcmp(blks[i].path, path)) {
			const char *disk = strcmp(blks[i].type, "disk") ? blks[i].pkname : blks[i].name;

			for (int j = 0; j < nblks; j++)
				if (!strcmp(blks[j].name, disk))
					return removable(&blks[j]);
		}
	return 0;
}

static void on_format(GtkButton *b, gpointer d)
{
	char path[64], fs[24];
	int kind, disk, ok;
	GtkWidget *dlg, *box, *grid, *type, *label, *note;
	const char *owner = getenv("MY_SETUP_USER");

	(void)b; (void)d;
	if (!chosen(path, sizeof path, &kind, fs, sizeof fs, &disk) || kind == K_SYSTEM_USED)
		return;
	dlg = gtk_dialog_new_with_buttons(_("Format"), GTK_WINDOW(win), GTK_DIALOG_MODAL,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("Format"), GTK_RESPONSE_OK, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 12);
	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	type = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(type), "vfat", _("FAT32 (works everywhere; files up to 4 GB)"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(type), "exfat", _("exFAT (Windows, Mac, cameras; big files)"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(type), "ext4", _("ext4 (Linux only; best for backups)"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(type), "ntfs", _("NTFS (Windows disks)"));
	gtk_combo_box_set_active(GTK_COMBO_BOX(type), 1);
	label = gtk_entry_new();
	gtk_entry_set_max_length(GTK_ENTRY(label), 16);
	gtk_entry_set_text(GTK_ENTRY(label), "USB");
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Filesystem")), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), type, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Name")), 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), label, 1, 1, 1, 1);
	gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
	note = gtk_label_new(disk ? _("The whole disk gets one new partition. Everything on it is deleted.") :
	                            _("Everything on this partition is deleted."));
	gtk_label_set_xalign(GTK_LABEL(note), 0);
	gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
	gtk_widget_show_all(dlg);
	ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
	if (ok) {
		char *t = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)));
		char *l = g_strdup(gtk_entry_get_text(GTK_ENTRY(label)));

		gtk_widget_destroy(dlg);
		/* names: letters, digits, - and _ (FAT and exFAT are picky) */
		for (char *p = l; *p; p++)
			if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
				*p = '_';
		/* the system's disk or an internal one: the big warning first */
		if ((kind == K_SYSTEM_DISK || !on_removable(path)) ? dangerous_ok(path, kind) : 1) {
			const char *argv[] = { TOOL, "format", path, t, l, owner ? owner : "", NULL };

			start_tool(argv);
		}
		g_free(t);
		g_free(l);
		return;
	}
	gtk_widget_destroy(dlg);
}

static void on_check(GtkButton *b, gpointer d)
{
	char path[64], fs[24];
	int kind, disk;
	const char *argv[] = { TOOL, "check", NULL, NULL };

	(void)b; (void)d;
	if (!chosen(path, sizeof path, &kind, fs, sizeof fs, &disk) || kind == K_SYSTEM_USED)
		return;
	argv[2] = path;
	start_tool(argv);
}

static void on_refresh(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	fill_tree();
	update_buttons();
}

static gboolean on_close(GtkWidget *w, GdkEvent *e, gpointer d)
{
	(void)w; (void)e; (void)d;
	return running;            /* not while formatting */
}

static int manage(void)
{
	GtkWidget *box, *scroll, *row, *refresh, *oscroll, *hint;
	GtkCellRenderer *r;
	GtkTreeViewColumn *col;
	const char *titles[] = { NULL, _("Disk / partition"), _("Size"), _("Filesystem"), _("Open in") };

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), _("Disks"));
	gtk_window_set_default_size(GTK_WINDOW(win), 720, 520);
	gtk_window_set_icon_name(GTK_WINDOW(win), "drive-harddisk");
	g_signal_connect(win, "delete-event", G_CALLBACK(on_close), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(win), box);

	store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
	                           G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
	tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	r = gtk_cell_renderer_pixbuf_new();
	col = gtk_tree_view_column_new_with_attributes("", r, "icon-name", COL_ICON, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
	for (int c = COL_NAME; c <= COL_WHERE; c++) {
		r = gtk_cell_renderer_text_new();
		col = gtk_tree_view_column_new_with_attributes(titles[c], r, "text", c, NULL);
		gtk_tree_view_column_set_resizable(col, TRUE);
		gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
	}
	g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), "changed",
	                 G_CALLBACK(on_select), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), tree);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	hint = gtk_label_new(_("Partitions the running system uses can't be changed. "
	                     "Formatting deletes everything on the disk or partition."));
	gtk_label_set_xalign(GTK_LABEL(hint), 0);
	gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
	gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
	gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	btn_format = gtk_button_new_with_label(_("Format…"));
	gtk_style_context_add_class(gtk_widget_get_style_context(btn_format), "destructive-action");
	g_signal_connect(btn_format, "clicked", G_CALLBACK(on_format), NULL);
	btn_check = gtk_button_new_with_label(_("Check and repair"));
	g_signal_connect(btn_check, "clicked", G_CALLBACK(on_check), NULL);
	refresh = gtk_button_new_with_label(_("Refresh"));
	g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), NULL);
	gtk_box_pack_start(GTK_BOX(row), btn_format, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), btn_check, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), refresh, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

	status_label = gtk_label_new(_("Choose a disk or partition."));
	gtk_label_set_xalign(GTK_LABEL(status_label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

	output = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(output), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(output), TRUE);
	oscroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_widget_set_size_request(oscroll, -1, 120);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(oscroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(oscroll), output);
	gtk_box_pack_start(GTK_BOX(box), oscroll, FALSE, FALSE, 0);

	fill_tree();
	gtk_widget_show_all(win);
	update_buttons();
	gtk_main();
	return 0;
}

/* --- Started by a normal user: ask for the root password ----------------------------- */

static GtkWidget *auth_err;

static void ask_root_password(void);

/* my-admin exits with 2 for a wrong password; otherwise it was the Disks
 * window, which has now been closed. */
static void on_admin_done(GPid pid, gint st, gpointer d)
{
	(void)d;
	g_spawn_close_pid(pid);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 2) {
		ask_root_password();
		gtk_label_set_text(GTK_LABEL(auth_err), _("Wrong password."));
		return;
	}
	gtk_main_quit();
}

static void on_auth(GtkDialog *dlg, gint response, gpointer entry)
{
	const char *argv[] = { "/usr/bin/my-admin", "--disks", NULL };
	char *pw;
	GPid pid;
	int in;

	if (response != GTK_RESPONSE_OK) {
		gtk_main_quit();
		return;
	}
	pw = g_strdup_printf("%s\n", gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(GTK_WIDGET(dlg));
	if (!g_spawn_async_with_pipes(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                              NULL, NULL, &pid, &in, NULL, NULL, NULL)) {
		g_free(pw);
		gtk_main_quit();
		return;
	}
	if (write(in, pw, strlen(pw)) < 0) {
		/* my-admin then reads nothing and says "wrong password" */
	}
	close(in);
	memset(pw, 0, strlen(pw));
	g_free(pw);
	g_child_watch_add(pid, on_admin_done, NULL);
}

static void ask_root_password(void)
{
	GtkWidget *d = gtk_dialog_new_with_buttons(_("Disks"), NULL, 0,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("OK"), GTK_RESPONSE_OK, NULL);
	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
	GtkWidget *l = gtk_label_new(_("Formatting and repairing disks needs the administrator "
	                             "(root) password."));
	GtkWidget *e = gtk_entry_new();

	gtk_window_set_icon_name(GTK_WINDOW(d), "drive-harddisk");
	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 44);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
	auth_err = gtk_label_new("");
	gtk_style_context_add_class(gtk_widget_get_style_context(auth_err), "error");
	gtk_box_pack_start(GTK_BOX(box), auth_err, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	g_signal_connect(d, "response", G_CALLBACK(on_auth), e);
	gtk_widget_show_all(d);
}

int main(int argc, char **argv)
{
	int manage_mode = argc > 1 && !strcmp(argv[1], "--manage");

	i18n_init();
	gtk_init(&argc, &argv);
	if (geteuid() == 0)
		return manage();
	if (manage_mode) {
		fprintf(stderr, "my-disks: --manage needs root\n");
		return 1;
	}
	ask_root_password();
	gtk_main();
	return 0;
}
