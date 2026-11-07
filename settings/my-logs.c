/*
 * my-logs: the log viewer of my-distro (GTK 3), "System logs" in the Start
 * menu. Shows what the system and its services wrote down:
 *
 *   System messages   /var/log/messages (syslogd: logins, Wi-Fi, VPN, updates…)
 *   Kernel            the kernel's messages (dmesg): hardware, drivers
 *   NAME.log          the other logs in /var/log (desktop, power, …)
 *
 * The search field keeps only the lines with that text in them (upper and
 * lower case don't matter). New lines appear by themselves.
 *
 * /var/log is in RAM (/tmp): the logs start empty at every boot.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_DIR  "/var/log"
#define MAX_READ (2 * 1024 * 1024)   /* big logs: only their end */

static GtkWidget *sources, *view, *search, *info, *scroll;
static GtkTextBuffer *buf;
static GtkTextTag *hit_tag;
static char *text;                    /* the whole log as read */
static char current[300];             /* path, or "" for the kernel */
static time_t cur_mtime;
static off_t cur_size;

/* The kernel's messages (dmesg reads them; allowed for every user) */
static char *read_kernel(void)
{
	const char *argv[] = { "/bin/dmesg", NULL };
	char *out = NULL;

	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL,
	                  NULL, NULL, &out, NULL, NULL, NULL))
		return g_strdup("Could not read the kernel's messages.");
	return out;
}

static char *read_log(const char *path)
{
	FILE *f = fopen(path, "r");
	struct stat st;
	char *data;
	size_t n;
	off_t start = 0;

	if (!f)
		return NULL;
	if (fstat(fileno(f), &st) == 0 && st.st_size > MAX_READ)
		start = st.st_size - MAX_READ;
	fseeko(f, start, SEEK_SET);
	data = g_malloc(MAX_READ + 1);
	n = fread(data, 1, MAX_READ, f);
	data[n] = '\0';
	fclose(f);
	if (start > 0) {                  /* begin at a whole line */
		char *nl = strchr(data, '\n');
		char *rest = g_strdup_printf("(only the last 2 MB)\n%s", nl ? nl + 1 : data);

		g_free(data);
		return rest;
	}
	return data;
}

static int at_bottom(void)
{
	GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));

	return gtk_adjustment_get_value(a) >= gtk_adjustment_get_upper(a) -
	       gtk_adjustment_get_page_size(a) - 20;
}

static gboolean scroll_to_end(gpointer d)
{
	GtkTextIter end;

	(void)d;
	gtk_text_buffer_get_end_iter(buf, &end);
	gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(view), &end, 0, FALSE, 0, 1);
	return G_SOURCE_REMOVE;
}

/* Shows the lines that contain the search text (all if it is empty) */
static void show(int keep_bottom)
{
	const char *q = gtk_entry_get_text(GTK_ENTRY(search));
	char *needle = g_utf8_casefold(q, -1), **lines;
	GString *out = g_string_new(NULL);
	int total = 0, shown = 0, bottom = keep_bottom || at_bottom();
	char *label;

	lines = g_strsplit(text ? text : "", "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *low;

		if (!lines[i][0] && !lines[i + 1])
			break;                        /* the empty end after the last \n */
		total++;
		if (needle[0]) {
			low = g_utf8_casefold(lines[i], -1);
			if (!strstr(low, needle)) {
				g_free(low);
				continue;
			}
			g_free(low);
		}
		g_string_append(out, lines[i]);
		g_string_append_c(out, '\n');
		shown++;
	}
	gtk_text_buffer_set_text(buf, out->str, out->len);

	/* mark the search text in what is shown */
	if (needle[0]) {
		GtkTextIter it, s, e;

		gtk_text_buffer_get_start_iter(buf, &it);
		while (gtk_text_iter_forward_search(&it, q, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &s, &e, NULL)) {
			gtk_text_buffer_apply_tag(buf, hit_tag, &s, &e);
			it = e;
		}
		label = g_strdup_printf("%d of %d lines", shown, total);
	} else {
		label = g_strdup_printf("%d lines", total);
	}
	gtk_label_set_text(GTK_LABEL(info), label);
	if (bottom)
		g_idle_add(scroll_to_end, NULL);
	g_free(label);
	g_strfreev(lines);
	g_string_free(out, TRUE);
	g_free(needle);
}

static void load(int keep_bottom)
{
	struct stat st;

	g_free(text);
	if (!current[0]) {
		text = read_kernel();
	} else if (!(text = read_log(current))) {
		text = g_strdup_printf(access(current, F_OK) == 0 ?
			"Only the administrator (root) may read %s." :
			"%s doesn't exist (yet).", current);
	}
	if (current[0] && stat(current, &st) == 0) {
		cur_mtime = st.st_mtime;
		cur_size = st.st_size;
	}
	show(keep_bottom);
}

/* Every 2 seconds: read again if the file has grown */
static gboolean poll_log(gpointer d)
{
	struct stat st;

	(void)d;
	if (current[0] && stat(current, &st) == 0 &&
	    (st.st_mtime != cur_mtime || st.st_size != cur_size))
		load(0);
	return G_SOURCE_CONTINUE;
}

static void on_source(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	const char *path;

	(void)l; (void)d;
	if (!r)
		return;
	path = g_object_get_data(G_OBJECT(r), "path");
	g_strlcpy(current, path ? path : "", sizeof current);
	load(1);
}

static void on_search(GtkSearchEntry *e, gpointer d)
{
	(void)e; (void)d;
	show(0);
}

static void on_refresh(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	load(0);
}

static void add_source(const char *label, const char *path)
{
	GtkWidget *row = gtk_list_box_row_new(), *l = gtk_label_new(label);

	gtk_label_set_xalign(GTK_LABEL(l), 0);
	g_object_set(l, "margin", 8, NULL);
	gtk_container_add(GTK_CONTAINER(row), l);
	g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path), g_free);
	gtk_list_box_insert(GTK_LIST_BOX(sources), row, -1);
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The logs in /var/log: files ending in .log that belong to root (/var/log
 * is /tmp, where anybody can make files) */
static void fill_sources(void)
{
	DIR *dir = opendir(LOG_DIR);
	struct dirent *e;
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);

	add_source("System messages", LOG_DIR "/messages");
	add_source("Kernel", NULL);
	while (dir && (e = readdir(dir))) {
		char path[300];
		struct stat st;
		size_t n = strlen(e->d_name);

		snprintf(path, sizeof path, LOG_DIR "/%s", e->d_name);
		if (n > 4 && strcmp(e->d_name + n - 4, ".log") == 0 &&
		    lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0)
			g_ptr_array_add(names, g_strdup(e->d_name));
	}
	if (dir)
		closedir(dir);
	qsort(names->pdata, names->len, sizeof(char *), by_name);
	for (guint i = 0; i < names->len; i++) {
		char path[300];

		snprintf(path, sizeof path, LOG_DIR "/%s", (char *)names->pdata[i]);
		add_source(names->pdata[i], path);
	}
	g_ptr_array_free(names, TRUE);
}

int main(int argc, char **argv)
{
	GtkWidget *win, *paned, *left, *right, *top, *refresh;
	GtkCssProvider *css = gtk_css_provider_new();

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "System logs");
	gtk_window_set_default_size(GTK_WINDOW(win), 900, 560);
	gtk_window_set_icon_name(GTK_WINDOW(win), "text-x-generic");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_container_add(GTK_CONTAINER(win), paned);

	sources = gtk_list_box_new();
	g_signal_connect(sources, "row-selected", G_CALLBACK(on_source), NULL);
	left = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(left), sources);
	gtk_widget_set_size_request(left, 180, -1);
	gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);

	right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(right), 10);
	top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	search = gtk_search_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search (only lines with this text)");
	g_signal_connect(search, "search-changed", G_CALLBACK(on_search), NULL);
	info = gtk_label_new("");
	gtk_style_context_add_class(gtk_widget_get_style_context(info), "dim-label");
	refresh = gtk_button_new_with_label("Refresh");
	g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), NULL);
	gtk_box_pack_start(GTK_BOX(top), search, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(top), info, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(top), refresh, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(right), top, FALSE, FALSE, 0);

	view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
	buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	hit_tag = gtk_text_buffer_create_tag(buf, "hit", "background", "#f5c211",
	                                     "foreground", "#000000", NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), view);
	gtk_box_pack_start(GTK_BOX(right), scroll, TRUE, TRUE, 0);
	gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);

	/* a slightly smaller font for the log text */
	gtk_css_provider_load_from_data(css, "textview { font-size: 9pt; }", -1, NULL);
	gtk_style_context_add_provider(gtk_widget_get_style_context(view),
	                               GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	fill_sources();
	gtk_widget_show_all(win);
	gtk_list_box_select_row(GTK_LIST_BOX(sources),
	                        gtk_list_box_get_row_at_index(GTK_LIST_BOX(sources), 0));
	gtk_widget_grab_focus(search);
	g_timeout_add_seconds(2, poll_log, NULL);
	gtk_main();
	return 0;
}
