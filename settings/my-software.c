/*
 * my-software: the Software app of my-distro (GTK 3), "Software" in the
 * Start menu. Installs and removes programs that are not in the system
 * image: a PDF viewer, Vim, Node.js, games, ...
 *
 * The work is done by my-pkg (desktop/overlay/usr/bin/my-pkg):
 *   - at the start, as you: "my-pkg refresh" downloads the signed package
 *     list, "my-pkg list --porcelain" says what is in it and what is
 *     installed (one tab-separated line per package)
 *   - Install / Remove / Update all: "my-admin --pkg install NAME" runs
 *     "my-pkg install NAME" as root after checking the root password, like
 *     "Users and system" does. The password is asked once and kept (in this
 *     program's memory only) until the window closes.
 * my-pkg's messages ("[2/6] Downloading vim...", "  40 %") move the progress
 * bar. Installed programs get a Start menu entry: my-pkg writes them to
 * /var/lib/my-pkg/menu and the taskbar reads that file again.
 *
 * Packages of the category "Libraries" are not shown: they come and go with
 * the programs that need them.
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MY_PKG    "/usr/bin/my-pkg"
#define MY_ADMIN  "/usr/bin/my-admin"
#define LIBRARIES "Libraries"

struct pkg {
	char *name, *version, *have, *category, *title, *desc;  /* have: NULL = not installed */
	gint64 size, disk;                                      /* download, on disk (bytes) */
	char **deps;
};

static const char *cat_order[] = { "Office", "Graphics", "Internet", "Programming",
	"Tools", "Games", "Sound", NULL };

static GPtrArray *pkgs;               /* struct pkg *, from my-pkg list --porcelain */
static GtkWidget *win, *cats, *list, *search, *status, *bar, *notice, *upgrade_btn;
static char shown_cat[64] = "";       /* "" = all, "*" = installed, else a category */
static char *password;                /* the root password, once it was right */
static int busy, is_root, live;
static GString *output;               /* what my-pkg said during the last action */
static char **last_args;              /* that action, to run again after a wrong password */

static void reload(void);
static void run_action(char **args);

/* --- The package list ---------------------------------------------------------------- */

static void free_pkg(gpointer p)
{
	struct pkg *k = p;

	g_free(k->name); g_free(k->version); g_free(k->have); g_free(k->category);
	g_free(k->title); g_free(k->desc); g_strfreev(k->deps);
	g_free(k);
}

static struct pkg *find(const char *name)
{
	for (guint i = 0; pkgs && i < pkgs->len; i++) {
		struct pkg *k = g_ptr_array_index(pkgs, i);

		if (strcmp(k->name, name) == 0)
			return k;
	}
	return NULL;
}

static char *human(gint64 bytes)
{
	if (bytes >= 1 << 20)
		return g_strdup_printf("%.1f MB", bytes / 1048576.0);
	return g_strdup_printf("%d KB", (int)((bytes + 1023) / 1024));
}

/* Reads "my-pkg list --porcelain": @key=value lines, then the packages */
static void load_list(void)
{
	const char *argv[] = { MY_PKG, "list", "--porcelain", NULL };
	char *out = NULL, **lines, sys[32] = "", idx[32] = "";

	if (pkgs)
		g_ptr_array_free(pkgs, TRUE);
	pkgs = g_ptr_array_new_with_free_func(free_pkg);
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, NULL, NULL) || !out)
		return;
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char **f;

		if (lines[i][0] == '@') {
			if (g_str_has_prefix(lines[i], "@system="))
				g_strlcpy(sys, lines[i] + 8, sizeof sys);
			else if (g_str_has_prefix(lines[i], "@index_system="))
				g_strlcpy(idx, lines[i] + 14, sizeof idx);
			else if (g_str_has_prefix(lines[i], "@live="))
				live = lines[i][6] == '1';
			continue;
		}
		f = g_strsplit(lines[i], "\t", 9);
		if (g_strv_length(f) == 9) {
			struct pkg *k = g_new0(struct pkg, 1);

			k->name = g_strdup(f[0]);
			k->version = g_strdup(f[1]);
			k->have = strcmp(f[2], "-") ? g_strdup(f[2]) : NULL;
			k->category = g_strdup(f[3]);
			k->size = g_ascii_strtoll(f[4], NULL, 10);
			k->disk = g_ascii_strtoll(f[5], NULL, 10);
			k->deps = strcmp(f[6], "-") ? g_strsplit(f[6], ",", -1) : g_new0(char *, 1);
			k->title = g_strdup(f[7]);
			k->desc = g_strdup(f[8]);
			g_ptr_array_add(pkgs, k);
		}
		g_strfreev(f);
	}
	g_strfreev(lines);
	g_free(out);

	/* What to say above the list */
	if (idx[0] && strcmp(idx, sys) != 0) {
		char *t = g_strdup_printf("These packages are made for my-distro %s, but this is "
			"my-distro %s: they can't be installed. Update the system (System update), "
			"or wait until the packages for %s are out.", idx, sys, sys);

		gtk_label_set_text(GTK_LABEL(notice), t);
		gtk_widget_show(notice);
		g_free(t);
	} else if (live) {
		gtk_label_set_text(GTK_LABEL(notice), "This is the live system: programs you install "
			"here go into RAM and are gone when you shut down, like everything else. "
			"Install my-distro to a disk to keep them.");
		gtk_widget_show(notice);
	} else {
		gtk_widget_hide(notice);
	}
}

/* --- What is shown ---------------------------------------------------------------------- */

static int matches(struct pkg *k)
{
	const char *q = gtk_entry_get_text(GTK_ENTRY(search));
	char *all, *hay, *needle, **words;
	int ok = 1;

	if (strcmp(k->category, LIBRARIES) == 0)
		return 0;
	if (strcmp(shown_cat, "*") == 0 ? !k->have : shown_cat[0] && strcmp(shown_cat, k->category))
		return 0;
	if (!q[0])
		return 1;
	all = g_strjoin(" ", k->name, k->title, k->desc, k->category, NULL);
	hay = g_utf8_casefold(all, -1);
	g_free(all);
	needle = g_utf8_casefold(q, -1);
	words = g_strsplit(needle, " ", -1);
	for (int i = 0; words[i]; i++)                    /* every word must be there */
		if (words[i][0] && !strstr(hay, words[i]))
			ok = 0;
	g_strfreev(words);
	g_free(needle);
	g_free(hay);
	return ok;
}

static void on_install(GtkButton *b, gpointer name);
static void on_remove(GtkButton *b, gpointer name);

static GtkWidget *row_for(struct pkg *k)
{
	GtkWidget *row = gtk_list_box_row_new(), *grid = gtk_grid_new(), *l, *btn, *buttons;
	char *t, *s1, *s2, needs[300] = "";

	gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
	g_object_set(grid, "margin", 10, NULL);

	t = g_markup_printf_escaped("<b>%s</b>  <span alpha=\"60%%\">%s %s%s</span>", k->title,
		k->name, k->have ? k->have : k->version,
		k->have && strcmp(k->have, k->version) ? "  (newer one available)" : "");
	l = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(l), t);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_set_hexpand(l, TRUE);
	gtk_grid_attach(GTK_GRID(grid), l, 0, 0, 1, 1);
	g_free(t);

	/* the programs it needs (libraries don't count) */
	for (int i = 0; k->deps[i]; i++) {
		struct pkg *d = find(k->deps[i]);

		if (d && strcmp(d->category, LIBRARIES) != 0) {
			g_strlcat(needs, needs[0] ? ", " : "\nNeeds: ", sizeof needs);
			g_strlcat(needs, d->title, sizeof needs);
		}
	}
	t = g_strdup_printf("%s%s", k->desc, needs);
	l = gtk_label_new(t);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(grid), l, 0, 1, 1, 1);
	g_free(t);

	s1 = human(k->disk);
	s2 = human(k->size);
	t = k->have ? g_strdup_printf("%s on disk", s1)
	            : g_strdup_printf("%s on disk\n%s download", s1, s2);
	l = gtk_label_new(t);
	gtk_label_set_xalign(GTK_LABEL(l), 1);
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_RIGHT);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(grid), l, 1, 0, 1, 2);
	g_free(t); g_free(s1); g_free(s2);

	buttons = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
	if (k->have) {
		btn = gtk_button_new_with_label("Remove");
		gtk_style_context_add_class(gtk_widget_get_style_context(btn), "destructive-action");
		g_signal_connect(btn, "clicked", G_CALLBACK(on_remove), k->name);
	} else {
		btn = gtk_button_new_with_label("Install");
		gtk_style_context_add_class(gtk_widget_get_style_context(btn), "suggested-action");
		g_signal_connect(btn, "clicked", G_CALLBACK(on_install), k->name);
	}
	gtk_widget_set_size_request(btn, 90, -1);
	gtk_box_pack_start(GTK_BOX(buttons), btn, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(grid), buttons, 2, 0, 1, 2);

	gtk_container_add(GTK_CONTAINER(row), grid);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	return row;
}

static void fill_list(void)
{
	GList *old = gtk_container_get_children(GTK_CONTAINER(list));
	int n = 0, newer = 0;

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
	for (guint i = 0; pkgs && i < pkgs->len; i++) {
		struct pkg *k = g_ptr_array_index(pkgs, i);

		if (k->have && strcmp(k->have, k->version) != 0)
			newer++;
		if (matches(k)) {
			gtk_list_box_insert(GTK_LIST_BOX(list), row_for(k), -1);
			n++;
		}
	}
	if (n == 0) {
		GtkWidget *l = gtk_label_new(strcmp(shown_cat, "*") == 0 ? "No programs installed yet."
			: pkgs && pkgs->len ? "Nothing found." : "No package list (are you online?).");

		g_object_set(l, "margin", 30, NULL);
		gtk_list_box_insert(GTK_LIST_BOX(list), l, -1);
	}
	gtk_widget_set_visible(upgrade_btn, newer > 0);
	gtk_widget_show_all(list);
}

static void add_category(const char *label, const char *id)
{
	GtkWidget *row = gtk_list_box_row_new(), *l = gtk_label_new(label);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	g_object_set(l, "margin", 8, NULL);
	gtk_container_add(GTK_CONTAINER(row), l);
	g_object_set_data_full(G_OBJECT(row), "id", g_strdup(id), g_free);
	gtk_list_box_insert(GTK_LIST_BOX(cats), row, -1);
}

/* All, Installed, then the categories in the list (known ones first) */
static void fill_categories(void)
{
	GList *old = gtk_container_get_children(GTK_CONTAINER(cats));
	GPtrArray *seen = g_ptr_array_new();

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
	add_category("All programs", "");
	add_category("Installed", "*");
	for (int i = 0; cat_order[i]; i++)
		for (guint j = 0; pkgs && j < pkgs->len; j++)
			if (strcmp(((struct pkg *)g_ptr_array_index(pkgs, j))->category, cat_order[i]) == 0) {
				add_category(cat_order[i], cat_order[i]);
				g_ptr_array_add(seen, (gpointer)cat_order[i]);
				break;
			}
	for (guint j = 0; pkgs && j < pkgs->len; j++) {
		const char *c = ((struct pkg *)g_ptr_array_index(pkgs, j))->category;
		int known = strcmp(c, LIBRARIES) == 0;

		for (guint s = 0; s < seen->len && !known; s++)
			known = strcmp(c, g_ptr_array_index(seen, s)) == 0;
		if (!known) {
			add_category(c, c);
			g_ptr_array_add(seen, (gpointer)c);
		}
	}
	g_ptr_array_free(seen, TRUE);
	gtk_widget_show_all(cats);
}

static void on_category(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)d;
	if (!r)
		return;
	g_strlcpy(shown_cat, g_object_get_data(G_OBJECT(r), "id"), sizeof shown_cat);
	fill_list();
}

static void on_search(GtkSearchEntry *e, gpointer d)
{
	(void)e; (void)d;
	fill_list();
}

static void reload(void)
{
	load_list();
	fill_list();
}

/* --- Installing and removing ---------------------------------------------------------------- */

static int ask(const char *question, const char *detail, const char *yes, int destructive)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", question), *b;
	int answer;

	if (detail && detail[0])
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail);
	gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
	b = gtk_dialog_add_button(GTK_DIALOG(d), yes, GTK_RESPONSE_ACCEPT);
	gtk_style_context_add_class(gtk_widget_get_style_context(b),
	                            destructive ? "destructive-action" : "suggested-action");
	answer = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	return answer == GTK_RESPONSE_ACCEPT;
}

/* The not yet installed packages NAME brings with it (and their sizes) */
static void needed(const char *name, GPtrArray *found, gint64 *disk)
{
	struct pkg *k = find(name);

	if (!k)
		return;
	for (guint i = 0; i < found->len; i++)
		if (g_ptr_array_index(found, i) == k)
			return;
	g_ptr_array_add(found, k);
	if (!k->have)
		*disk += k->disk;
	for (int i = 0; k->deps[i]; i++)
		needed(k->deps[i], found, disk);
}

static void on_install(GtkButton *b, gpointer name)
{
	struct pkg *k = find(name);
	GPtrArray *all = g_ptr_array_new();
	GString *more = g_string_new(NULL);
	gint64 disk = 0;
	char *q, *s, *detail;
	char *args[] = { "install", (char *)name, NULL };

	(void)b;
	needed(name, all, &disk);
	for (guint i = 1; i < all->len; i++) {
		struct pkg *d = g_ptr_array_index(all, i);

		if (!d->have)
			g_string_append_printf(more, "%s%s", more->len ? ", " : "", d->title);
	}
	s = human(disk);
	q = g_strdup_printf("Install %s?", k->title);
	detail = g_strdup_printf("It needs %s of space.%s%s%s", s,
		more->len ? " It brings: " : "", more->str, more->len ? "." : "");
	if (ask(q, detail, "Install", 0))
		run_action(args);
	g_free(q); g_free(s); g_free(detail);
	g_string_free(more, TRUE);
	g_ptr_array_free(all, TRUE);
}

/* Installed programs that need NAME, directly or through others */
static void dependents(const char *name, GPtrArray *found)
{
	for (guint i = 0; i < pkgs->len; i++) {
		struct pkg *k = g_ptr_array_index(pkgs, i);
		int dup = 0;

		if (!k->have || !g_strv_contains((const char *const *)k->deps, name))
			continue;
		for (guint j = 0; j < found->len; j++)
			dup |= g_ptr_array_index(found, j) == k;
		if (!dup) {
			g_ptr_array_add(found, k);
			dependents(k->name, found);
		}
	}
}

static void on_remove(GtkButton *b, gpointer name)
{
	struct pkg *k = find(name);
	GPtrArray *users = g_ptr_array_new();
	GString *who = g_string_new(NULL);
	char *q, *detail;
	char *plain[] = { "remove", (char *)name, NULL };
	char *with[] = { "remove", "--with-dependents", (char *)name, NULL };

	(void)b;
	dependents(name, users);
	for (guint i = 0; i < users->len; i++)
		g_string_append_printf(who, "%s%s", i ? ", " : "",
		                       ((struct pkg *)g_ptr_array_index(users, i))->title);
	q = g_strdup_printf("Remove %s?", k->title);
	detail = users->len ? g_strdup_printf("%s needs it, so it is removed too.", who->str)
	                    : g_strdup("Your own files made with it stay.");
	if (ask(q, detail, users->len ? "Remove both" : "Remove", 1))
		run_action(users->len ? with : plain);
	g_free(q); g_free(detail);
	g_string_free(who, TRUE);
	g_ptr_array_free(users, TRUE);
}

static void on_upgrade(GtkButton *b, gpointer d)
{
	char *args[] = { "upgrade", NULL };

	(void)b; (void)d;
	if (ask("Install the newer versions?", "Programs that have a newer version are "
	        "downloaded and replaced.", "Update all", 0))
		run_action(args);
}

/* Asks for the root password (modal). NULL = cancelled. */
static char *ask_password(const char *error)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Software", GTK_WINDOW(win), GTK_DIALOG_MODAL,
		"Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);
	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d)), *entry, *l;
	char *pw = NULL;

	gtk_container_set_border_width(GTK_CONTAINER(box), 18);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	l = gtk_label_new("Installing and removing programs needs the administrator (root) "
	                  "password.");
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
	gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	entry = gtk_entry_new();
	gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
	if (error) {
		l = gtk_label_new(error);
		gtk_style_context_add_class(gtk_widget_get_style_context(l), "error");
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
	}
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
		pw = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
	gtk_widget_destroy(d);
	return pw;
}

static void set_busy(int on)
{
	busy = on;
	gtk_widget_set_sensitive(list, !on);
	gtk_widget_set_sensitive(upgrade_btn, !on);
	gtk_widget_set_visible(bar, on);
	if (on)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), 0);
}

/* A line from my-pkg: "[2/6] Downloading vim..." starts step 2 of 6,
 * "  40 %  (1 of 3 MB)" is how far that download is */
static int step, steps;

static void progress_line(char *line)
{
	int a, b, pct;

	g_string_append(output, line);
	g_strstrip(line);
	if (sscanf(line, "[%d/%d]", &a, &b) == 2 && b > 0) {
		char *text = strchr(line, ']') + 1;

		step = a;
		steps = b;
		gtk_label_set_text(GTK_LABEL(status), g_strchug(text));
	} else if (sscanf(line, "%d %%", &pct) == 1 && steps) {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
			(step - 1 + pct / 100.0) / steps);
		return;
	} else if (line[0]) {
		gtk_label_set_text(GTK_LABEL(status), line);
	}
	if (steps)
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), (double)(step - 1) / steps);
}

static gboolean on_output(GIOChannel *ch, GIOCondition cond, gpointer d)
{
	char *line;
	gsize n;

	(void)d;
	if (cond & G_IO_IN)
		while (g_io_channel_read_line(ch, &line, &n, NULL, NULL) == G_IO_STATUS_NORMAL) {
			progress_line(line);
			g_free(line);
		}
	return !(cond & (G_IO_HUP | G_IO_ERR));
}

static void on_done(GPid pid, gint st, gpointer d)
{
	(void)d;
	g_spawn_close_pid(pid);
	set_busy(0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 2 && !is_root) {    /* my-admin: wrong password */
		char *again = ask_password("Wrong password.");

		g_free(password);
		password = again;
		if (password) {
			run_action(last_args);
			return;
		}
		gtk_label_set_text(GTK_LABEL(status), "Nothing was changed.");
		return;
	}
	reload();
	if (g_spawn_check_wait_status(st, NULL)) {
		gtk_label_set_text(GTK_LABEL(status), "Done. Installed programs are in the "
		                   "Start menu, or start them in a terminal.");
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), 1);
	} else {
		GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "That did not work.");

		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", output->str);
		gtk_dialog_run(GTK_DIALOG(m));
		gtk_widget_destroy(m);
		gtk_label_set_text(GTK_LABEL(status), "That did not work: nothing was changed, "
		                   "or only part of it (see the message).");
	}
}

/* Runs "my-admin --pkg ARGS..." with the root password on its input */
static void run_action(char **args)
{
	GPtrArray *argv = g_ptr_array_new();
	GIOChannel *ch;
	GPid pid;
	int in, out;
	char *pw;

	if (!is_root && !password && !(password = ask_password(NULL)))
		return;
	if (args != last_args) {
		g_strfreev(last_args);
		last_args = g_strdupv(args);
	}
	g_ptr_array_add(argv, MY_ADMIN);
	g_ptr_array_add(argv, "--pkg");
	for (int i = 0; last_args[i]; i++)
		g_ptr_array_add(argv, last_args[i]);
	g_ptr_array_add(argv, NULL);
	if (!g_spawn_async_with_pipes(NULL, (char **)argv->pdata, NULL,
	                              G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDERR_TO_DEV_NULL,
	                              NULL, NULL, &pid, &in, &out, NULL, NULL)) {
		gtk_label_set_text(GTK_LABEL(status), "Could not start my-admin.");
		g_ptr_array_free(argv, TRUE);
		return;
	}
	g_ptr_array_free(argv, TRUE);
	pw = g_strdup_printf("%s\n", password ? password : "");
	if (write(in, pw, strlen(pw)) < 0) {
		/* my-admin then reads nothing and says "wrong password" */
	}
	close(in);
	memset(pw, 0, strlen(pw));
	g_free(pw);

	g_string_truncate(output, 0);
	step = steps = 0;
	set_busy(1);
	gtk_label_set_text(GTK_LABEL(status), "Working…");
	ch = g_io_channel_unix_new(out);
	g_io_channel_set_flags(ch, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_add_watch(ch, G_IO_IN | G_IO_HUP | G_IO_ERR, on_output, NULL);
	g_io_channel_unref(ch);
	g_child_watch_add(pid, on_done, NULL);
}

/* Closing while my-pkg works would cut it off in the middle */
static gboolean on_close(GtkWidget *w, GdkEvent *e, gpointer d)
{
	(void)w; (void)e; (void)d;
	if (busy)
		gtk_label_set_text(GTK_LABEL(status), "Please wait until this is finished.");
	return busy;
}

/* --- Start: download the list in the background ------------------------------------------- */

static void on_refreshed(GPid pid, gint st, gpointer d)
{
	(void)d;
	g_spawn_close_pid(pid);
	set_busy(0);
	reload();
	fill_categories();
	gtk_list_box_select_row(GTK_LIST_BOX(cats),
	                        gtk_list_box_get_row_at_index(GTK_LIST_BOX(cats), 0));
	gtk_label_set_text(GTK_LABEL(status), g_spawn_check_wait_status(st, NULL) ? ""
		: pkgs && pkgs->len ? "Could not download the package list (no internet?): "
		                      "showing the last one."
		: "Could not download the package list (no internet?).");
}

static void start_refresh(void)
{
	const char *argv[] = { MY_PKG, "refresh", NULL };
	GPid pid;

	gtk_label_set_text(GTK_LABEL(status), "Downloading the package list…");
	set_busy(1);
	gtk_progress_bar_pulse(GTK_PROGRESS_BAR(bar));
	if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD |
	                   G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
	                   NULL, NULL, &pid, NULL)) {
		on_refreshed(0, 1 << 8, NULL);
		return;
	}
	g_child_watch_add(pid, on_refreshed, NULL);
}

int main(int argc, char **argv)
{
	GtkWidget *paned, *left, *right, *top, *scroll, *bottom;

	gtk_init(&argc, &argv);
	is_root = geteuid() == 0;
	output = g_string_new(NULL);

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Software");
	gtk_window_set_default_size(GTK_WINDOW(win), 920, 600);
	gtk_window_set_icon_name(GTK_WINDOW(win), "system-software-install");
	g_signal_connect(win, "delete-event", G_CALLBACK(on_close), NULL);
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_container_add(GTK_CONTAINER(win), paned);

	/* left: categories */
	cats = gtk_list_box_new();
	g_signal_connect(cats, "row-selected", G_CALLBACK(on_category), NULL);
	left = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(left), cats);
	gtk_widget_set_size_request(left, 160, -1);
	gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);

	/* right: search, notice, the programs, progress */
	right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(right), 10);
	top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search programs");
	g_signal_connect(search, "search-changed", G_CALLBACK(on_search), NULL);
	upgrade_btn = gtk_button_new_with_label("Update all");
	g_signal_connect(upgrade_btn, "clicked", G_CALLBACK(on_upgrade), NULL);
	gtk_widget_set_no_show_all(upgrade_btn, TRUE);
	gtk_box_pack_start(GTK_BOX(top), search, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(top), upgrade_btn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), top, FALSE, FALSE, 0);

	notice = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(notice), 0);
	gtk_label_set_line_wrap(GTK_LABEL(notice), TRUE);
	gtk_widget_set_no_show_all(notice, TRUE);
	gtk_box_pack_start(GTK_BOX(right), notice, FALSE, FALSE, 0);

	list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_box_pack_start(GTK_BOX(right), scroll, TRUE, TRUE, 0);

	bottom = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_END);
	bar = gtk_progress_bar_new();
	gtk_widget_set_no_show_all(bar, TRUE);
	gtk_box_pack_start(GTK_BOX(bottom), bar, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bottom), status, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), bottom, FALSE, FALSE, 0);
	gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);

	gtk_widget_show_all(win);
	start_refresh();
	gtk_main();
	if (password)
		memset(password, 0, strlen(password));
	return 0;
}
