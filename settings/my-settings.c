/*
 * my-settings: the Settings app of my-distro (GTK 3).
 *
 * Every change is saved at once to ~/.config/my-distro/settings (see
 * settings.h). The taskbar and the Clock app watch that file and follow by
 * themselves. The rest of the desktop is told here (apply_desktop):
 *   - Openbox (window borders and titles): we write a theme in the current
 *     colors to ~/.themes/my-distro and run `openbox --reconfigure`
 *   - GTK apps (file manager, browser, this app): light or dark style in
 *     ~/.config/gtk-3.0/settings.ini, used by apps started after the change
 *   - the wallpaper: PCManFM draws it, `pcmanfm --set-wallpaper` changes it
 *   - terminal colors: X resources for xterm, loaded with `xrdb -merge`
 *
 * `my-settings --apply` only does apply_desktop and exits; Openbox's
 * autostart runs it at every login. Run it yourself after editing the
 * settings file by hand.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings.h"

static struct settings cfg;
static int building;               /* 1 while filling in the widgets */

static GtkWidget *theme_buttons[16], *accent_switch, *accent_button;
static GtkWidget *wall_radio[3], *wall_file, *wall_mode, *wall_preview;

/* --- Applying the settings to the rest of the desktop ----------------------- */

/* mkdir -p for a path inside the home folder */
static void make_dirs(const char *path)
{
	char tmp[512];

	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

static FILE *open_in_home(const char *dir, const char *file)
{
	char path[512];

	snprintf(path, sizeof path, "%s/%s", g_get_home_dir(), dir);
	make_dirs(path);
	snprintf(path, sizeof path, "%s/%s/%s", g_get_home_dir(), dir, file);
	return fopen(path, "w");
}

/* Openbox theme: window titles, borders and Openbox's own menus. */
static void write_openbox_theme(const struct palette *p)
{
	FILE *f = open_in_home(".themes/my-distro/openbox-3", "themerc");

	if (!f)
		return;
	fprintf(f,
		"# Written by my-settings from the theme \"%s\". Changes are overwritten.\n"
		"border.width: 1\npadding.width: 8\npadding.height: 5\n"
		"window.handle.width: 0\nwindow.client.padding.width: 0\n"
		"window.client.padding.height: 0\nwindow.label.text.justify: left\n"
		"window.active.border.color: %s\nwindow.inactive.border.color: %s\n"
		"window.active.title.bg: flat solid\nwindow.active.title.bg.color: %s\n"
		"window.inactive.title.bg: flat solid\nwindow.inactive.title.bg.color: %s\n"
		"window.active.title.separator.color: %s\n"
		"window.inactive.title.separator.color: %s\n"
		"window.*.label.bg: parentrelative\n"
		"window.active.label.text.color: %s\nwindow.inactive.label.text.color: %s\n"
		"window.*.button.*.bg: parentrelative\n"
		"window.active.button.*.image.color: %s\n"
		"window.inactive.button.*.image.color: %s\n"
		"window.*.button.hover.bg: flat solid\nwindow.*.button.hover.bg.color: %s\n"
		"window.*.button.pressed.bg: flat solid\nwindow.*.button.pressed.bg.color: %s\n"
		"window.*.handle.bg: flat solid\nwindow.*.handle.bg.color: %s\n"
		"window.*.grip.bg: flat solid\nwindow.*.grip.bg.color: %s\n"
		"menu.border.color: %s\nmenu.separator.color: %s\n"
		"menu.title.bg: flat solid\nmenu.title.bg.color: %s\n"
		"menu.title.text.color: %s\nmenu.title.text.justify: left\n"
		"menu.items.bg: flat solid\nmenu.items.bg.color: %s\n"
		"menu.items.text.color: %s\nmenu.items.disabled.text.color: %s\n"
		"menu.items.active.bg: flat solid\nmenu.items.active.bg.color: %s\n"
		"menu.items.active.text.color: %s\n"
		"osd.border.color: %s\nosd.bg: flat solid\nosd.bg.color: %s\n"
		"osd.label.bg: parentrelative\nosd.label.text.color: %s\n"
		"osd.hilight.bg: flat solid\nosd.hilight.bg.color: %s\n"
		"osd.unhilight.bg: flat solid\nosd.unhilight.bg.color: %s\n",
		p->name, p->accent, p->sep, p->bg, p->bg, p->accent, p->sep,
		p->text, p->dim, p->text, p->dim, p->hover, p->active,
		p->bg, p->bg, p->accent, p->sep, p->button, p->accent,
		p->bg, p->text, p->dim, p->hover, p->text,
		p->accent, p->bg, p->text, p->accent, p->button);
	fclose(f);
}

static void write_gtk_settings(const struct settings *s)
{
	FILE *f = open_in_home(".config/gtk-3.0", "settings.ini");

	if (!f)
		return;
	fprintf(f, "# Written by my-settings.\n[Settings]\n"
		"gtk-theme-name=Adwaita\ngtk-application-prefer-dark-theme=%d\n"
		"gtk-icon-theme-name=Adwaita\ngtk-font-name=DejaVu Sans %d\n",
		s->pal.dark, s->font_size);
	fclose(f);
}

/* xterm's colors and font size. New terminals use them. */
static void write_xresources(const struct settings *s)
{
	char path[512], *cmd, *q;
	FILE *f = open_in_home(".config/my-distro", "xresources");

	if (!f)
		return;
	fprintf(f, "! Written by my-settings.\nXTerm*background: %s\nXTerm*foreground: %s\n"
		"XTerm*cursorColor: %s\nXTerm*faceSize: %d\n",
		s->pal.bg, s->pal.text, s->pal.accent, s->font_size + 1);
	fclose(f);
	snprintf(path, sizeof path, "%s/.config/my-distro/xresources", g_get_home_dir());
	q = g_shell_quote(path);
	cmd = g_strdup_printf("xrdb -merge %s", q);
	g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);   /* before xterm starts */
	g_free(cmd);
	g_free(q);
}

/* Wallpaper "color": a small picture in that color, stretched. */
static void plain_picture(const struct settings *s, char *path, int len)
{
	unsigned r = 0, g = 0, b = 0;
	GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 16, 16);
	char dir[512];

	sscanf(s->pal.bg + 1, "%2x%2x%2x", &r, &g, &b);
	gdk_pixbuf_fill(pb, r << 24 | g << 16 | b << 8 | 0xff);
	snprintf(dir, sizeof dir, "%s/.cache/my-distro", g_get_home_dir());
	make_dirs(dir);
	snprintf(path, len, "%s/plain-%s.png", dir, s->pal.bg + 1);
	gdk_pixbuf_save(pb, path, "png", NULL, NULL);
	g_object_unref(pb);
}

/* PCManFM's desktop settings; read when its desktop starts (at login). */
static void write_pcmanfm_desktop(const struct settings *s, const char *file,
                                  const char *mode)
{
	FILE *f = open_in_home(".config/pcmanfm/default", "desktop-items-0.conf");

	if (!f)
		return;
	fprintf(f, "[*]\nwallpaper_mode=%s\nwallpaper=%s\nwallpaper_common=1\n"
		"desktop_bg=%s\ndesktop_fg=%s\ndesktop_shadow=%s\n"
		"desktop_font=DejaVu Sans %d\nshow_wm_menu=1\nsort=mtime;ascending;\n"
		"show_documents=0\nshow_trash=0\nshow_mounts=0\n",
		mode, file, s->pal.bg, s->pal.dark ? "#ffffff" : "#12302f",
		s->pal.dark ? "#000000" : "#ffffff", s->font_size);
	fclose(f);
}

/* Is PCManFM (which draws the wallpaper) running for this user? */
static int pcmanfm_running(void)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int found = 0;

	while (d && !found && (e = readdir(d))) {
		char path[300], name[64] = "";
		struct stat st;
		FILE *f;

		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		if (stat(path, &st) < 0 || st.st_uid != getuid() || !(f = fopen(path, "r")))
			continue;
		if (fgets(name, sizeof name, f))
			found = strcmp(name, "pcmanfm\n") == 0;
		fclose(f);
	}
	if (d)
		closedir(d);
	return found;
}

static void apply_desktop(const struct settings *s)
{
	char file[512], cmd[1200];
	const char *mode = s->wallpaper_mode;

	write_openbox_theme(&s->pal);
	write_gtk_settings(s);
	write_xresources(s);

	settings_wallpaper_file(s, file, sizeof file);
	if (!file[0]) {
		plain_picture(s, file, sizeof file);
		mode = "stretch";
	}
	write_pcmanfm_desktop(s, file, mode);

	if (pcmanfm_running()) {
		/* At login it isn't running yet and reads the file above when it
		 * starts. Later it must be told. */
		char *q = g_shell_quote(file);

		snprintf(cmd, sizeof cmd, "pcmanfm --set-wallpaper=%s --wallpaper-mode=%s",
		         q, mode);
		g_free(q);
		g_spawn_command_line_async(cmd, NULL);
	}
	g_spawn_command_line_async("openbox --reconfigure", NULL);   /* reload the theme */
}

/* --- Reacting to the widgets --------------------------------------------- */

static void update_widgets(void);

/* Something changed: save, read back (fills in the theme's colors), apply. */
static void changed(void)
{
	if (building)
		return;
	settings_save(&cfg);
	settings_load(&cfg, NULL);
	apply_desktop(&cfg);
	update_widgets();
}

static void on_switch(GObject *o, GParamSpec *ps, gpointer field)
{
	(void)ps;
	*(int *)field = gtk_switch_get_active(GTK_SWITCH(o));
	changed();
}

static void on_spin(GtkSpinButton *b, gpointer field)
{
	*(int *)field = gtk_spin_button_get_value_as_int(b);
	changed();
}

static void on_combo_index(GtkComboBox *c, gpointer field)
{
	*(int *)field = gtk_combo_box_get_active(c);
	changed();
}

/* Combo whose item ids are the values of a text field of cfg */
static void on_combo_text(GtkComboBox *c, gpointer field)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (id)          /* both text fields of cfg used here have 32 bytes */
		g_strlcpy(field, id, 32);
	changed();
}

static void on_theme(GtkToggleButton *b, gpointer name)
{
	if (!gtk_toggle_button_get_active(b))
		return;
	g_strlcpy(cfg.theme, name, sizeof cfg.theme);
	changed();
}

static void color_to_hex(GtkColorChooser *c, char *hex)
{
	GdkRGBA rgba;

	gtk_color_chooser_get_rgba(c, &rgba);
	snprintf(hex, 8, "#%02x%02x%02x", (int)(rgba.red * 255 + 0.5),
	         (int)(rgba.green * 255 + 0.5), (int)(rgba.blue * 255 + 0.5));
}

static void on_accent(void)
{
	if (building)
		return;
	if (gtk_switch_get_active(GTK_SWITCH(accent_switch)))
		color_to_hex(GTK_COLOR_CHOOSER(accent_button), cfg.accent);
	else
		cfg.accent[0] = '\0';
	changed();
}

static void on_accent_switch(GObject *o, GParamSpec *ps, gpointer d)
{
	(void)o; (void)ps; (void)d;
	on_accent();
}

static void on_wallpaper(void)
{
	char *file;

	if (building)
		return;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(wall_radio[0]))) {
		strcpy(cfg.wallpaper, "theme");
	} else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(wall_radio[1]))) {
		strcpy(cfg.wallpaper, "color");
	} else {
		file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(wall_file));
		if (!file)                     /* no picture chosen yet */
			return;
		g_strlcpy(cfg.wallpaper, file, sizeof cfg.wallpaper);
		g_free(file);
	}
	changed();
}

static void on_wallpaper_file(GtkFileChooserButton *b, gpointer d)
{
	(void)b; (void)d;
	if (!building)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wall_radio[2]), TRUE);
	on_wallpaper();
}

static void on_reset(GtkButton *b, gpointer d)
{
	char path[512];

	(void)b; (void)d;
	settings_user_path(path, sizeof path, NULL);
	remove(path);
	settings_load(&cfg, NULL);
	settings_save(&cfg);
	apply_desktop(&cfg);
	update_widgets();
}

/* A small preview of a theme: its background with a taskbar and accent. */
static gboolean draw_swatch(GtkWidget *w, cairo_t *cr, gpointer data)
{
	const struct palette *p = data;
	int width = gtk_widget_get_allocated_width(w);
	int height = gtk_widget_get_allocated_height(w);
	GdkRGBA c;

	gdk_rgba_parse(&c, p->active);
	gdk_cairo_set_source_rgba(cr, &c);
	cairo_paint(cr);
	gdk_rgba_parse(&c, p->bg);
	gdk_cairo_set_source_rgba(cr, &c);
	cairo_rectangle(cr, 0, height - 14, width, 14);
	cairo_fill(cr);
	gdk_rgba_parse(&c, strcmp(p->name, cfg.theme) == 0 ? cfg.pal.accent : p->accent);
	gdk_cairo_set_source_rgba(cr, &c);
	cairo_rectangle(cr, 0, height - 14, width, 2);
	cairo_fill(cr);
	cairo_rectangle(cr, 6, height - 10, 18, 6);
	cairo_fill(cr);
	gdk_rgba_parse(&c, p->text);
	gdk_cairo_set_source_rgba(cr, &c);
	cairo_rectangle(cr, width - 22, height - 9, 16, 4);
	cairo_fill(cr);
	return TRUE;
}

/* --- Building the window --------------------------------------------------- */

static GtkWidget *page(GtkWidget *stack, const char *name, const char *title)
{
	GtkWidget *grid = gtk_grid_new(), *scroll;

	gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
	gtk_container_set_border_width(GTK_CONTAINER(grid), 24);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), grid);
	gtk_stack_add_titled(GTK_STACK(stack), scroll, name, title);
	return grid;
}

static void row(GtkWidget *grid, int y, const char *label, GtkWidget *w)
{
	GtkWidget *l = gtk_label_new(label);

	gtk_widget_set_halign(l, GTK_ALIGN_START);
	gtk_widget_set_halign(w, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), l, 0, y, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), w, 1, y, 1, 1);
}

static GtkWidget *heading(GtkWidget *grid, int y, const char *text)
{
	GtkWidget *l = gtk_label_new(NULL);
	char *m = g_markup_printf_escaped("<b>%s</b>", text);

	gtk_label_set_markup(GTK_LABEL(l), m);
	g_free(m);
	gtk_widget_set_halign(l, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(grid), l, 0, y, 2, 1);
	return l;
}

static GtkWidget *new_switch(int *field)
{
	GtkWidget *s = gtk_switch_new();

	g_object_set_data(G_OBJECT(s), "field", field);
	g_signal_connect(s, "notify::active", G_CALLBACK(on_switch), field);
	return s;
}

static GtkWidget *new_spin(int *field, int lo, int hi)
{
	GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, 1);

	g_object_set_data(G_OBJECT(s), "field", field);
	g_signal_connect(s, "value-changed", G_CALLBACK(on_spin), field);
	return s;
}

/* A drop-down list; the chosen item's number goes into *field. */
static GtkWidget *new_choice(int *field, const char *a, const char *b)
{
	GtkWidget *c = gtk_combo_box_text_new();

	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), a);
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c), b);
	g_object_set_data(G_OBJECT(c), "field", field);
	g_signal_connect(c, "changed", G_CALLBACK(on_combo_index), field);
	return c;
}

static GList *int_widgets;          /* widgets bound to an int of cfg */
static GtkWidget *date_combo;

static GtkWidget *bind(GtkWidget *w)
{
	int_widgets = g_list_append(int_widgets, w);
	return w;
}

static void build_appearance(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "appearance", "Colors");
	GtkWidget *flow = gtk_flow_box_new();
	GSList *group = NULL;

	heading(g, 0, "Theme");
	gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
	gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 3);
	gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 3);
	gtk_widget_set_hexpand(flow, TRUE);
	gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);
	gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);
	for (int i = 0; themes[i].name; i++) {
		GtkWidget *b = gtk_radio_button_new(group);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		GtkWidget *sw = gtk_drawing_area_new();

		group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(b));
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(b), FALSE);  /* a button */
		gtk_widget_set_size_request(sw, 110, 62);
		g_signal_connect(sw, "draw", G_CALLBACK(draw_swatch), (gpointer)&themes[i]);
		gtk_box_pack_start(GTK_BOX(box), sw, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(box), gtk_label_new(themes[i].label), FALSE, FALSE, 0);
		gtk_container_add(GTK_CONTAINER(b), box);
		g_signal_connect(b, "toggled", G_CALLBACK(on_theme), (gpointer)themes[i].name);
		gtk_container_add(GTK_CONTAINER(flow), b);
		theme_buttons[i] = b;
	}
	gtk_grid_attach(GTK_GRID(g), flow, 0, 1, 2, 1);

	heading(g, 2, "Accent color");
	accent_switch = gtk_switch_new();
	g_signal_connect(accent_switch, "notify::active", G_CALLBACK(on_accent_switch), NULL);
	row(g, 3, "Own accent color", accent_switch);
	accent_button = gtk_color_button_new();
	g_signal_connect(accent_button, "color-set", G_CALLBACK(on_accent), NULL);
	row(g, 4, "Color", accent_button);
}

static void build_wallpaper(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "wallpaper", "Wallpaper");
	GtkFileFilter *images = gtk_file_filter_new();

	heading(g, 0, "Wallpaper");
	wall_radio[0] = gtk_radio_button_new_with_label(NULL, "Matching the theme");
	wall_radio[1] = gtk_radio_button_new_with_label_from_widget(
		GTK_RADIO_BUTTON(wall_radio[0]), "Plain color");
	wall_radio[2] = gtk_radio_button_new_with_label_from_widget(
		GTK_RADIO_BUTTON(wall_radio[0]), "Picture:");
	for (int i = 0; i < 3; i++) {
		g_signal_connect(wall_radio[i], "toggled", G_CALLBACK(on_wallpaper), NULL);
		gtk_grid_attach(GTK_GRID(g), wall_radio[i], 0, 1 + i, 1, 1);
	}
	wall_file = gtk_file_chooser_button_new("Choose a picture",
	                                        GTK_FILE_CHOOSER_ACTION_OPEN);
	gtk_file_filter_set_name(images, "Pictures");
	gtk_file_filter_add_mime_type(images, "image/*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(wall_file), images);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(wall_file),
	                                    "/usr/share/backgrounds");
	g_signal_connect(wall_file, "file-set", G_CALLBACK(on_wallpaper_file), NULL);
	gtk_grid_attach(GTK_GRID(g), wall_file, 1, 3, 1, 1);

	wall_mode = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "crop", "Fill the screen");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "fit", "Fit, keep all of it");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "stretch", "Stretch");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "center", "Centre");
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "tile", "Tile");
	g_signal_connect(wall_mode, "changed", G_CALLBACK(on_combo_text), cfg.wallpaper_mode);
	row(g, 4, "Size", wall_mode);

	wall_preview = gtk_image_new();
	gtk_widget_set_halign(wall_preview, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(g), wall_preview, 0, 5, 2, 1);
}

static void build_clock(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "clock", "Clock");

	heading(g, 0, "Style");
	row(g, 1, "Taskbar clock", bind(new_choice(&cfg.panel_clock, "Digital", "Analog")));
	row(g, 2, "Clock app", bind(new_choice(&cfg.clock_app, "Digital", "Analog")));
	heading(g, 3, "Format");
	row(g, 4, "24-hour time", bind(new_switch(&cfg.clock_24h)));
	row(g, 5, "Show seconds", bind(new_switch(&cfg.clock_seconds)));
	row(g, 6, "Show the date", bind(new_switch(&cfg.clock_date)));
	date_combo = gtk_combo_box_text_new();
	g_signal_connect(date_combo, "changed", G_CALLBACK(on_combo_text), cfg.date_format);
	row(g, 7, "Date format", date_combo);
}

static void build_taskbar(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "taskbar", "Taskbar");

	heading(g, 0, "Taskbar");
	row(g, 1, "Position", bind(new_choice(&cfg.panel_position, "Bottom", "Top")));
	row(g, 2, "Height (pixels)", bind(new_spin(&cfg.panel_height, 28, 56)));
	row(g, 3, "Text size", bind(new_spin(&cfg.font_size, 8, 16)));
}

/* Puts the values of cfg into the widgets (without reacting to it). */
static void update_widgets(void)
{
	time_t now = time(NULL);
	struct tm tm;
	char example[64];
	GdkRGBA rgba;
	GdkPixbuf *pb;
	char file[512];

	building = 1;
	localtime_r(&now, &tm);

	for (int i = 0; themes[i].name; i++) {
		if (strcmp(themes[i].name, cfg.theme) == 0)
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(theme_buttons[i]), TRUE);
		gtk_widget_queue_draw(theme_buttons[i]);
	}
	gtk_switch_set_active(GTK_SWITCH(accent_switch), cfg.accent[0] != '\0');
	gdk_rgba_parse(&rgba, cfg.pal.accent);
	gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(accent_button), &rgba);
	gtk_widget_set_sensitive(accent_button, cfg.accent[0] != '\0');

	for (GList *l = int_widgets; l; l = l->next) {
		int *field = g_object_get_data(l->data, "field");

		if (GTK_IS_SWITCH(l->data))
			gtk_switch_set_active(l->data, *field);
		else if (GTK_IS_SPIN_BUTTON(l->data))
			gtk_spin_button_set_value(l->data, *field);
		else
			gtk_combo_box_set_active(l->data, *field);
	}

	/* date formats, each shown as today's date in that format */
	gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(date_combo));
	for (int i = 0; date_formats[i]; i++) {
		strftime(example, sizeof example, date_formats[i], &tm);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(date_combo), date_formats[i], example);
	}
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(date_combo), cfg.date_format)) {
		/* a format typed into the file by hand */
		settings_format_date(&cfg, &tm, example, sizeof example);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(date_combo), cfg.date_format, example);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(date_combo), cfg.date_format);
	}
	gtk_widget_set_sensitive(date_combo, cfg.clock_date);

	/* wallpaper */
	if (strcmp(cfg.wallpaper, "theme") == 0)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wall_radio[0]), TRUE);
	else if (strcmp(cfg.wallpaper, "color") == 0)
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wall_radio[1]), TRUE);
	else {
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wall_radio[2]), TRUE);
		gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(wall_file), cfg.wallpaper);
	}
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(wall_mode), cfg.wallpaper_mode);
	gtk_widget_set_sensitive(wall_mode, strcmp(cfg.wallpaper, "color") != 0);

	settings_wallpaper_file(&cfg, file, sizeof file);
	pb = file[0] ? gdk_pixbuf_new_from_file_at_scale(file, 320, 180, TRUE, NULL) : NULL;
	if (!pb) {                                /* plain color: a filled box */
		unsigned r = 0, g = 0, b = 0;

		sscanf(cfg.pal.bg + 1, "%2x%2x%2x", &r, &g, &b);
		pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 320, 180);
		gdk_pixbuf_fill(pb, r << 24 | g << 16 | b << 8 | 0xff);
	}
	gtk_image_set_from_pixbuf(GTK_IMAGE(wall_preview), pb);
	g_object_unref(pb);

	building = 0;
}

int main(int argc, char **argv)
{
	GtkWidget *win, *box, *side, *stack, *vbox, *bottom, *reset;

	if (argc > 1 && strcmp(argv[1], "--apply") == 0) {
		/* at login: no window, so GTK is only needed for the pictures */
		settings_load(&cfg, NULL);
		apply_desktop(&cfg);
		return 0;
	}

	gtk_init(&argc, &argv);
	settings_load(&cfg, NULL);

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Settings");
	gtk_window_set_default_size(GTK_WINDOW(win), 640, 560);
	gtk_window_set_icon_name(GTK_WINDOW(win), "preferences-desktop");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(stack),
	                              GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	build_appearance(stack);
	build_wallpaper(stack);
	build_clock(stack);
	build_taskbar(stack);

	side = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
	gtk_widget_set_size_request(side, 150, -1);

	reset = gtk_button_new_with_label("Reset to defaults");
	g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), NULL);
	bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_container_set_border_width(GTK_CONTAINER(bottom), 12);
	gtk_box_pack_end(GTK_BOX(bottom), reset, FALSE, FALSE, 0);

	vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(vbox), stack, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), bottom, FALSE, FALSE, 0);

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_pack_start(GTK_BOX(box), side, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_VERTICAL),
	                   FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), vbox, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(win), box);

	update_widgets();
	gtk_widget_show_all(win);
	gtk_main();
	return 0;
}
