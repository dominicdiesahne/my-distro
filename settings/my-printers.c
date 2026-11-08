/*
 * my-printers: the Printers window of my-distro (GTK 3), Start menu
 * "Printers". Also scans (Scan… button).
 *
 * CUPS (cupsd, /etc/init.d/S81cupsd) does the printing. Like the Wi-Fi window
 * with wpa_cli, this window uses CUPS's command-line tools:
 *   lpstat -p -d / lpstat -v    the printers, which one is the default
 *   lpinfo -l -v                printers it can find (USB, and in the network
 *                               with Avahi)
 *   lpinfo --device-id ID -m    drivers that fit a printer
 *   lpadmin                     add (-p), remove (-x), make default (-d)
 *   lp                          print a test page
 * Members of the group lpadmin may add and remove printers (cupsd.conf).
 *
 * Network printers of the last ~10 years need no driver: CUPS asks them what
 * they can do ("everywhere", IPP Everywhere / AirPrint). USB printers get
 * a driver from Gutenprint or HPLIP when one fits.
 *
 * Scanning: SANE's scanimage, with sane-airscan for network scanners.
 */
#include <ctype.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_PAGE "/usr/share/cups/data/default-testpage.pdf"   /* from cups-filters */
#define WEB_PAGE  "http://localhost:631/"

struct printer {
	char name[64];
	char info[128];       /* description */
	char state[64];       /* idle, printing, disabled */
	int is_default;
};

static GtkWidget *win, *list, *status_label, *btn_remove, *btn_default, *btn_test;
static struct printer printers[32];
static int nprinters;

/* Runs a command (argv, no shell) and returns its output (g_free it).
 * ok = 1 when it ended with 0. Errors go into the output too. */
static char *run(const char *const *argv, int *ok)
{
	char *out = NULL, *err = NULL, *all;
	int st = -1;

	*ok = 0;
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
	                  &out, &err, &st, NULL))
		return g_strdup("could not start it");
	*ok = g_spawn_check_wait_status(st, NULL);
	all = g_strconcat(out ? out : "", err ? err : "", NULL);
	g_free(out);
	g_free(err);
	return g_strstrip(all);
}

static void message(GtkMessageType type, const char *title, const char *text)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, type,
	                                      GTK_BUTTONS_CLOSE, "%s", title);

	if (text && text[0])
		gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

/* --- The list of printers --------------------------------------------------------------- */

static void read_printers(void)
{
	const char *ps[] = { "lpstat", "-l", "-p", NULL }, *ds[] = { "lpstat", "-d", NULL };
	char *out, *def, **lines, defname[64] = "";
	int ok;
	struct printer *cur = NULL;

	nprinters = 0;
	def = run(ds, &ok);           /* "system default destination: PDF" */
	if (ok && strchr(def, ':'))
		g_strlcpy(defname, g_strstrip(strchr(def, ':') + 1), sizeof defname);
	g_free(def);

	/* printer PDF is idle.  enabled since ...
	 *         Description: Save as PDF */
	out = run(ps, &ok);
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char name[64], state[64];

		if (sscanf(lines[i], "printer %63s %63[^.]", name, state) == 2 &&
		    nprinters < (int)G_N_ELEMENTS(printers)) {
			cur = &printers[nprinters++];
			memset(cur, 0, sizeof *cur);
			g_strlcpy(cur->name, name, sizeof cur->name);
			/* "is idle", "now printing PDF-3", "disabled since ..." */
			g_strlcpy(cur->state, g_str_has_prefix(state, "is ") ? state + 3 :
			          g_str_has_prefix(state, "now printing") ? "printing" :
			          g_str_has_prefix(state, "disabled") ? "switched off" : state,
			          sizeof cur->state);
			cur->is_default = !strcmp(name, defname);
		} else if (cur && strstr(lines[i], "Description:")) {
			g_strlcpy(cur->info, g_strstrip(strstr(lines[i], ":") + 1), sizeof cur->info);
		}
	}
	g_strfreev(lines);
	g_free(out);
}

static struct printer *selected(void)
{
	GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	int i = r ? gtk_list_box_row_get_index(r) : -1;

	return i >= 0 && i < nprinters ? &printers[i] : NULL;
}

static void update_buttons(void)
{
	struct printer *p = selected();

	gtk_widget_set_sensitive(btn_remove, p != NULL);
	gtk_widget_set_sensitive(btn_default, p && !p->is_default);
	gtk_widget_set_sensitive(btn_test, p != NULL);
}

static void fill_list(void)
{
	GList *old = gtk_container_get_children(GTK_CONTAINER(list));

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
	read_printers();
	for (int i = 0; i < nprinters; i++) {
		struct printer *p = &printers[i];
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		GtkWidget *icon = gtk_image_new_from_icon_name("printer", GTK_ICON_SIZE_LARGE_TOOLBAR);
		GtkWidget *name = gtk_label_new(NULL), *info = gtk_label_new(NULL);
		char *m = g_markup_printf_escaped("<b>%s</b>%s\n<small>%s</small>", p->info[0] ? p->info : p->name,
		                                  p->is_default ? "   (default)" : "", p->name);

		gtk_label_set_markup(GTK_LABEL(name), m);
		gtk_label_set_xalign(GTK_LABEL(name), 0);
		gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
		gtk_label_set_text(GTK_LABEL(info), p->state);
		gtk_style_context_add_class(gtk_widget_get_style_context(info), "dim-label");
		g_object_set(row, "margin", 8, NULL);
		gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
		gtk_box_pack_end(GTK_BOX(row), info, FALSE, FALSE, 0);
		gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
		g_free(m);
	}
	gtk_widget_show_all(list);
	gtk_label_set_text(GTK_LABEL(status_label), nprinters ? "" :
		"No printers yet. Click Add printer. (Is the print service running? "
		"It starts at boot: /etc/init.d/S81cupsd)");
	update_buttons();
}

static void on_row(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)r; (void)d;
	update_buttons();
}

/* lpadmin's own message, or ours when the user may not do it */
static const char *why(const char *out)
{
	return strstr(out, "Forbidden") || strstr(out, "Unauthorized") ?
		"Your user may not change printers yet (it is not in the group lpadmin). "
		"As root: addgroup YOUR-NAME lpadmin, then log in again." : out;
}

static void on_remove(GtkButton *b, gpointer d)
{
	struct printer *p = selected();
	const char *argv[] = { "lpadmin", "-x", NULL, NULL };
	char *out, *q;
	int ok;
	GtkWidget *dlg;

	(void)b; (void)d;
	if (!p)
		return;
	q = g_strdup_printf("Remove the printer \"%s\"?", p->info[0] ? p->info : p->name);
	dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
	                             GTK_BUTTONS_OK_CANCEL, "%s", q);
	g_free(q);
	ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
	gtk_widget_destroy(dlg);
	if (!ok)
		return;
	argv[2] = p->name;
	out = run(argv, &ok);
	if (!ok)
		message(GTK_MESSAGE_ERROR, "Could not remove the printer.", why(out));
	g_free(out);
	fill_list();
}

static void on_default(GtkButton *b, gpointer d)
{
	struct printer *p = selected();
	const char *argv[] = { "lpadmin", "-d", NULL, NULL };
	char *out;
	int ok;

	(void)b; (void)d;
	if (!p)
		return;
	argv[2] = p->name;
	out = run(argv, &ok);
	if (!ok)
		message(GTK_MESSAGE_ERROR, "Could not make it the default printer.", why(out));
	g_free(out);
	fill_list();
}

static void on_test(GtkButton *b, gpointer d)
{
	struct printer *p = selected();
	const char *argv[] = { "lp", "-d", NULL, "-t", "Test page", TEST_PAGE, NULL };
	char *out;
	int ok;

	(void)b; (void)d;
	if (!p)
		return;
	argv[2] = p->name;
	out = run(argv, &ok);
	message(ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
	        ok ? "The test page is on its way." : "Could not print.", out);
	g_free(out);
	fill_list();
}

static void on_web(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	g_spawn_command_line_async("netsurf-gtk3 " WEB_PAGE, NULL);
}

/* --- Adding a printer ------------------------------------------------------------------- */

struct found {
	char uri[512];
	char info[160];
	char model[160];      /* make-and-model */
	char devid[512];      /* IEEE 1284 device id: MFG:...;MDL:...; */
	int network;
};

static struct found found[48];
static int nfound;
static GtkWidget *add_dlg, *add_list, *add_status, *add_address, *add_ok;

/* lpinfo -l -v prints blocks like
 *   Device: uri = usb://HP/LaserJet?serial=12
 *           class = direct
 *           info = HP LaserJet
 *           make-and-model = HP LaserJet Pro M15w
 *           device-id = MFG:HP;MDL:LaserJet Pro M15w;
 * Only real printers are kept (with "://" and a name), not the bare
 * protocols ("ipp", "socket") that are listed too. */
static void parse_devices(const char *out)
{
	char **lines = g_strsplit(out, "\n", -1);
	struct found *f = NULL;

	nfound = 0;
	for (int i = 0; lines[i]; i++) {
		char *l = g_strstrip(lines[i]), *v = strstr(l, " = ");

		if (!v)
			continue;
		v += 3;
		if (g_str_has_prefix(l, "Device: uri")) {
			f = NULL;
			if (strstr(v, "://") && nfound < (int)G_N_ELEMENTS(found)) {
				f = &found[nfound++];
				memset(f, 0, sizeof *f);
				g_strlcpy(f->uri, v, sizeof f->uri);
				f->network = !g_str_has_prefix(v, "usb:") && !g_str_has_prefix(v, "hp:");
			}
		} else if (f && g_str_has_prefix(l, "info"))
			g_strlcpy(f->info, v, sizeof f->info);
		else if (f && g_str_has_prefix(l, "make-and-model"))
			g_strlcpy(f->model, v, sizeof f->model);
		else if (f && g_str_has_prefix(l, "device-id"))
			g_strlcpy(f->devid, v, sizeof f->devid);
	}
	g_strfreev(lines);
	/* the same printer can come over several ways (dnssd, ipp, ...):
	 * keep the first of each name */
	for (int i = 0; i < nfound; i++)
		for (int j = i + 1; j < nfound; j++)
			if (found[i].info[0] && !strcmp(found[i].info, found[j].info)) {
				memmove(&found[j], &found[j + 1], (nfound - j - 1) * sizeof found[0]);
				nfound--;
				j--;
			}
}

static void on_devices(GObject *src, GAsyncResult *res, gpointer d)
{
	char *out = NULL;

	(void)d;
	if (!add_dlg)                 /* closed meanwhile */
		return;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL);
	parse_devices(out ? out : "");
	g_free(out);
	for (int i = 0; i < nfound; i++) {
		GtkWidget *l = gtk_label_new(NULL);
		char *esc = g_markup_escape_text(found[i].info[0] ? found[i].info : found[i].uri, -1);
		char *t = g_strdup_printf("%s\n<small>%s</small>", esc,
		                          found[i].network ? "in the network" : "USB");

		gtk_label_set_markup(GTK_LABEL(l), t);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		g_object_set(l, "margin", 6, NULL);
		gtk_list_box_insert(GTK_LIST_BOX(add_list), l, -1);
		g_free(t);
		g_free(esc);
	}
	gtk_widget_show_all(add_list);
	gtk_label_set_text(GTK_LABEL(add_status), nfound ?
		"Choose your printer, or type its address below." :
		"No printer found. Is it switched on, and in the same network (or plugged in)? "
		"You can also type its address (IP or name) below.");
}

static void start_search(void)
{
	GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
		"lpinfo", "--timeout", "10", "-l", "-v", NULL);

	gtk_label_set_text(GTK_LABEL(add_status), "Looking for printers (10 seconds)…");
	if (p) {
		g_subprocess_communicate_utf8_async(p, NULL, NULL, on_devices, NULL);
		g_object_unref(p);
	}
}

/* A printer name for CUPS: letters, digits, - and _ (at most 40) */
static void queue_name(const char *from, char *out, size_t len)
{
	size_t n = 0;

	for (const char *s = from; *s && n + 1 < len && n < 40; s++)
		if (isalnum((unsigned char)*s) || *s == '-' || *s == '_')
			out[n++] = *s;
		else if (n && out[n - 1] != '_')
			out[n++] = '_';
	while (n && out[n - 1] == '_')
		n--;
	out[n] = '\0';
	if (!n)
		g_strlcpy(out, "Printer", len);
}

/* The best driver for a printer that isn't driverless: lpinfo --device-id
 * lists the fitting ones as "NAME DESCRIPTION". NULL = none. */
static char *find_driver(const struct found *f)
{
	const char *argv[] = { "lpinfo", "--device-id", f->devid, "-m", NULL };
	char *out, *best = NULL, **lines;
	int ok;

	if (!f->devid[0])
		return NULL;
	out = run(argv, &ok);
	lines = g_strsplit(out, "\n", -1);
	/* recommended ones first (hpcups, gutenprint's "Simplified"), else the first */
	for (int pass = 0; pass < 2 && !best; pass++)
		for (int i = 0; lines[i] && !best; i++) {
			char *sp = strchr(lines[i], ' ');

			if (!sp || !strchr(lines[i], ':'))
				continue;
			if (pass == 1 || strstr(lines[i], "recommended") || strstr(lines[i], "hpcups"))
				best = g_strndup(lines[i], sp - lines[i]);
		}
	g_strfreev(lines);
	g_free(out);
	return best;
}

static int name_taken(const char *name)
{
	for (int k = 0; k < nprinters; k++)
		if (!g_ascii_strcasecmp(printers[k].name, name))
			return 1;
	return 0;
}

/* Adds it with lpadmin; returns 1 if it worked (or shows why not).
 * f = what the search found, NULL for an address typed in. */
static int add_printer(const char *uri, const char *info, const struct found *f)
{
	char base[48], name[56], *models[2] = { NULL, NULL }, *out = NULL;
	int ok = 0, n = 0;

	queue_name(info[0] ? info : uri, base, sizeof base);
	g_strlcpy(name, base, sizeof name);
	for (int i = 2; name_taken(name) && i < 100; i++)      /* a free name */
		snprintf(name, sizeof name, "%s_%d", base, i);

	/* Network printers: driverless first. Then a driver, if one fits. */
	if (!f || f->network)
		models[n++] = g_strdup("everywhere");
	if (f && (models[n] = find_driver(f)))
		n++;
	if (n == 0) {
		message(GTK_MESSAGE_ERROR, "my-distro has no driver for this printer.",
		        "Printers made since about 2015 work without a driver over the network "
		        "(Wi-Fi or cable): connect it to your network and try again.");
		return 0;
	}
	gtk_label_set_text(GTK_LABEL(add_status), "Adding the printer…");
	while (gtk_events_pending())
		gtk_main_iteration();

	for (int i = 0; i < n && !ok; i++) {
		const char *argv[] = { "lpadmin", "-p", name, "-E", "-v", uri, "-m", models[i],
		                       "-D", info[0] ? info : name, "-o", "printer-is-shared=false", NULL };

		g_free(out);
		out = run(argv, &ok);
		if (strstr(out, "Forbidden") || strstr(out, "Unauthorized"))
			break;
	}
	if (!ok)
		message(GTK_MESSAGE_ERROR, "Could not add the printer.",
		        strstr(out, "IPP Everywhere") || strstr(out, "everywhere") ?
		        "It doesn't answer as a driverless printer (IPP Everywhere). Check "
		        "the address; some older printers only work over their USB cable." : why(out));
	g_free(out);
	g_free(models[0]);
	g_free(models[1]);
	return ok;
}

static void on_add(GtkButton *b, gpointer d)
{
	GtkWidget *box, *scroll, *row;
	int added = 0;

	(void)b; (void)d;
	add_dlg = gtk_dialog_new_with_buttons("Add printer", GTK_WINDOW(win), GTK_DIALOG_MODAL,
		"Cancel", GTK_RESPONSE_CANCEL, "Add", GTK_RESPONSE_OK, NULL);
	gtk_window_set_default_size(GTK_WINDOW(add_dlg), 480, 400);
	box = gtk_dialog_get_content_area(GTK_DIALOG(add_dlg));
	gtk_container_set_border_width(GTK_CONTAINER(box), 12);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	add_status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(add_status), 0);
	gtk_label_set_line_wrap(GTK_LABEL(add_status), TRUE);
	gtk_box_pack_start(GTK_BOX(box), add_status, FALSE, FALSE, 0);
	add_list = gtk_list_box_new();
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), add_list);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Or its address:"), FALSE, FALSE, 0);
	add_address = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(add_address), "e.g. 192.168.1.20 or printer.local");
	gtk_box_pack_start(GTK_BOX(row), add_address, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	add_ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(add_dlg), GTK_RESPONSE_OK);
	gtk_widget_show_all(add_dlg);
	start_search();

	while (!added && gtk_dialog_run(GTK_DIALOG(add_dlg)) == GTK_RESPONSE_OK) {
		const char *addr = gtk_entry_get_text(GTK_ENTRY(add_address));
		GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(add_list));

		if (addr[0]) {
			/* only a host name or address: no spaces, slashes or quotes */
			char *uri;

			if (strpbrk(addr, " /\"'\\")) {
				gtk_label_set_text(GTK_LABEL(add_status), "Type only the address, like 192.168.1.20.");
				continue;
			}
			uri = g_strdup_printf("ipp://%s/ipp/print", addr);
			added = add_printer(uri, addr, NULL);
			g_free(uri);
		} else if (r && gtk_list_box_row_get_index(r) < nfound) {
			struct found *f = &found[gtk_list_box_row_get_index(r)];

			added = add_printer(f->uri, f->info[0] ? f->info : f->model, f);
		} else {
			gtk_label_set_text(GTK_LABEL(add_status), "Choose a printer, or type its address.");
		}
	}
	gtk_widget_destroy(add_dlg);
	add_dlg = NULL;
	fill_list();
	if (added && nprinters)
		gtk_label_set_text(GTK_LABEL(status_label), "Added. Print a test page to try it.");
}

/* --- Scanning ----------------------------------------------------------------------------- */

static GtkWidget *scan_dlg, *scan_combo, *scan_res, *scan_mode, *scan_status;
static char *scan_file;

static void on_scanners(GObject *src, GAsyncResult *res, gpointer d)
{
	char *out = NULL, **lines;
	int n = 0;

	(void)d;
	if (!scan_dlg)
		return;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL);
	/* scanimage -f "%d|%v %m%n": device|vendor model */
	lines = g_strsplit(out ? out : "", "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *bar = strchr(lines[i], '|');

		if (!bar)
			continue;
		*bar = '\0';
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(scan_combo), lines[i], bar + 1);
		n++;
	}
	g_strfreev(lines);
	g_free(out);
	gtk_combo_box_set_active(GTK_COMBO_BOX(scan_combo), 0);
	gtk_label_set_text(GTK_LABEL(scan_status), n ? "" :
		"No scanner found. Is it switched on, and plugged in or in the same network?");
	gtk_dialog_set_response_sensitive(GTK_DIALOG(scan_dlg), GTK_RESPONSE_OK, n > 0);
}

static void on_scanned(GObject *src, GAsyncResult *res, gpointer d)
{
	char *err = NULL;
	int ok;

	(void)d;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, NULL, &err, NULL);
	ok = g_subprocess_get_if_exited(G_SUBPROCESS(src)) && g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
	if (ok) {
		const char *argv[] = { "my-notify", "-i", "scanner", "Scanned",
		                       scan_file, NULL };

		g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
	}
	if (scan_dlg) {
		gtk_label_set_text(GTK_LABEL(scan_status), ok ? "Done: the picture is in your Pictures folder." :
		                   err && err[0] ? err : "The scan did not work.");
		gtk_dialog_set_response_sensitive(GTK_DIALOG(scan_dlg), GTK_RESPONSE_OK, TRUE);
	} else if (!ok) {
		unlink(scan_file);
	}
	g_free(err);
}

static void on_scan_response(GtkDialog *dlg, int response, gpointer d)
{
	GSubprocess *p;
	char stamp[32], *dir, *res, *dev;
	time_t now = time(NULL);

	(void)d;
	if (response != GTK_RESPONSE_OK) {
		gtk_widget_destroy(GTK_WIDGET(dlg));
		scan_dlg = NULL;
		return;
	}
	dev = gtk_combo_box_get_active_id(GTK_COMBO_BOX(scan_combo)) ?
	      g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(scan_combo))) : NULL;
	if (!dev)
		return;
	strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", localtime(&now));
	dir = g_build_filename(g_get_home_dir(), "Pictures", NULL);
	g_mkdir_with_parents(dir, 0755);
	g_free(scan_file);
	scan_file = g_strdup_printf("%s/scan-%s.png", dir, stamp);
	res = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(scan_res)));
	p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, NULL,
		"scanimage", "-d", dev, "--format=png", "--resolution", res,
		"--mode", gtk_combo_box_get_active_id(GTK_COMBO_BOX(scan_mode)),
		"-o", scan_file, NULL);
	if (p) {
		gtk_label_set_text(GTK_LABEL(scan_status), "Scanning…");
		gtk_dialog_set_response_sensitive(dlg, GTK_RESPONSE_OK, FALSE);
		g_subprocess_communicate_utf8_async(p, NULL, NULL, on_scanned, NULL);
		g_object_unref(p);
	}
	g_free(dir);
	g_free(res);
	g_free(dev);
}

static void on_scan(GtkButton *b, gpointer d)
{
	GtkWidget *box, *grid;
	GSubprocess *p;

	(void)b; (void)d;
	if (scan_dlg) {
		gtk_window_present(GTK_WINDOW(scan_dlg));
		return;
	}
	scan_dlg = gtk_dialog_new_with_buttons("Scan", GTK_WINDOW(win), 0,
		"Close", GTK_RESPONSE_CLOSE, "Scan", GTK_RESPONSE_OK, NULL);
	box = gtk_dialog_get_content_area(GTK_DIALOG(scan_dlg));
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_box_set_spacing(GTK_BOX(box), 10);
	grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
	scan_combo = gtk_combo_box_text_new();
	scan_res = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(scan_res), "150", "150 dpi (screen, e-mail)");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(scan_res), "300", "300 dpi (documents)");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(scan_res), "600", "600 dpi (photos)");
	gtk_combo_box_set_active(GTK_COMBO_BOX(scan_res), 1);
	scan_mode = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(scan_mode), "Color", "Color");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(scan_mode), "Gray", "Gray");
	gtk_combo_box_set_active(GTK_COMBO_BOX(scan_mode), 0);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Scanner"), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), scan_combo, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Quality"), 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), scan_res, 1, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Colors"), 0, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), scan_mode, 1, 2, 1, 1);
	gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
	scan_status = gtk_label_new("Looking for scanners…");
	gtk_label_set_line_wrap(GTK_LABEL(scan_status), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(scan_status), 50);
	gtk_box_pack_start(GTK_BOX(box), scan_status, FALSE, FALSE, 0);
	gtk_dialog_set_response_sensitive(GTK_DIALOG(scan_dlg), GTK_RESPONSE_OK, FALSE);
	g_signal_connect(scan_dlg, "response", G_CALLBACK(on_scan_response), NULL);
	gtk_widget_show_all(scan_dlg);

	p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
	                     "scanimage", "-f", "%d|%v %m%n", NULL);
	if (p) {
		g_subprocess_communicate_utf8_async(p, NULL, NULL, on_scanners, NULL);
		g_object_unref(p);
	}
}

static GtkWidget *button(const char *text, GCallback cb)
{
	GtkWidget *b = gtk_button_new_with_label(text);

	g_signal_connect(b, "clicked", cb, NULL);
	return b;
}

int main(int argc, char **argv)
{
	GtkWidget *box, *scroll, *row, *add;

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Printers");
	gtk_window_set_default_size(GTK_WINDOW(win), 600, 420);
	gtk_window_set_icon_name(GTK_WINDOW(win), "printer");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(win), box);

	list = gtk_list_box_new();
	g_signal_connect(list, "row-selected", G_CALLBACK(on_row), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	status_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status_label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	add = button("Add printer…", G_CALLBACK(on_add));
	gtk_style_context_add_class(gtk_widget_get_style_context(add), "suggested-action");
	btn_remove = button("Remove", G_CALLBACK(on_remove));
	btn_default = button("Make default", G_CALLBACK(on_default));
	btn_test = button("Test page", G_CALLBACK(on_test));
	gtk_box_pack_start(GTK_BOX(row), add, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), btn_remove, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), btn_default, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), btn_test, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), button("Scan…", G_CALLBACK(on_scan)), FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), button("More settings", G_CALLBACK(on_web)), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

	gtk_widget_show_all(win);
	fill_list();
	gtk_main();
	return 0;
}
