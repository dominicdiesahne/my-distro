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
 *   - screens (Display page): xrandr sets resolution and arrangement now;
 *     the same xrandr options are saved in ~/.config/my-distro/display and
 *     set again at every login
 *   - size of text and icons: Xft.dpi in the X resources (programs started
 *     afterwards; everything at the next login)
 *
 *   - Accessibility page: high contrast (two themes in settings.c, and GTK's
 *     HighContrast style), the size of text (the same setting as on the
 *     Display page) and of the mouse pointer (Xcursor.size, GTK's
 *     gtk-cursor-theme-size), and buttons for the magnifier (my-zoom), read
 *     aloud (my-speak) and the on-screen keyboard (my-keyboard); 0.9.7: the
 *     screen reader (my-reader) on or off
 *   - Permissions page (0.9.7): camera, microphone and home folder per
 *     program, applied by my-run when the Start menu starts it
 *
 * `my-settings --apply` only does apply_desktop and exits; Openbox's
 * autostart runs it at every login. Run it yourself after editing the
 * settings file by hand. `my-settings --page display` opens the window on
 * that page (--page access: Accessibility).
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings.h"
#include "i18n.h"

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

/* GTK's look: Adwaita light or dark, or its high-contrast style (built into
 * GTK 3); the font size and the mouse pointer's size */
static void write_gtk_settings(const struct settings *s)
{
	FILE *f = open_in_home(".config/gtk-3.0", "settings.ini");

	if (!f)
		return;
	fprintf(f, "# Written by my-settings.\n[Settings]\n"
		"gtk-theme-name=%s\ngtk-application-prefer-dark-theme=%d\n"
		"gtk-icon-theme-name=Adwaita\ngtk-font-name=DejaVu Sans %d\n"
		"gtk-cursor-theme-name=Adwaita\ngtk-cursor-theme-size=%d\n",
		!settings_high_contrast(s) ? "Adwaita" : s->pal.dark ? "HighContrastInverse" : "HighContrast",
		s->pal.dark, s->font_size, s->cursor_size);
	fclose(f);
}

/* xterm's colors and font size, the size of text and of the mouse pointer.
 * New programs use them. */
static void write_xresources(const struct settings *s)
{
	char path[512], *cmd, *q;
	FILE *f = open_in_home(".config/my-distro", "xresources");

	if (!f)
		return;
	/* Xft.dpi: how big text is drawn (96 = 100 %); GTK apps, xterm, the
	 * taskbar all follow it. Xcursor: the mouse pointer of X programs. */
	fprintf(f, "! Written by my-settings.\nXTerm*background: %s\nXTerm*foreground: %s\n"
		"XTerm*cursorColor: %s\nXTerm*faceSize: %d\nXft.dpi: %d\n"
		"Xcursor.theme: Adwaita\nXcursor.size: %d\n",
		s->pal.bg, s->pal.text, s->pal.accent, s->font_size + 1, 96 * s->scale / 100,
		s->cursor_size);
	fclose(f);
	snprintf(path, sizeof path, "%s/.config/my-distro/xresources", g_get_home_dir());
	q = g_shell_quote(path);
	cmd = g_strdup_printf("xrdb -merge %s", q);
	g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);   /* before xterm starts */
	/* the pointer over the desktop's background, in the new size */
	g_spawn_command_line_sync("xsetroot -cursor_name left_ptr", NULL, NULL, NULL, NULL);
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

/* ~/.config/my-distro/display: the xrandr options chosen on the Display page */
static void display_file(char *path, int len)
{
	snprintf(path, len, "%s/.config/my-distro/display", g_get_home_dir());
}

/* Runs xrandr with these options (a string like "--output X --mode 1280x800") */
static int run_xrandr(const char *opts)
{
	char *cmd = g_strdup_printf("xrandr %s", opts);
	int st = -1;

	g_spawn_command_line_sync(cmd, NULL, NULL, &st, NULL);
	g_free(cmd);
	return st == 0;
}

/* At login: the screens as they were chosen last time */
static void apply_display(void)
{
	char path[512], *opts = NULL;

	display_file(path, sizeof path);
	if (g_file_get_contents(path, &opts, NULL, NULL)) {
		g_strstrip(opts);
		if (opts[0] && opts[0] != '#')
			run_xrandr(opts);      /* fails harmlessly if a screen is gone */
		g_free(opts);
	}
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

/* Users, passwords, computer name, keyboard: my-setup (asks for root). */
static void on_users(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	g_spawn_command_line_async("my-setup", NULL);
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
	GtkWidget *g = page(stack, "appearance", _("Colors"));
	GtkWidget *flow = gtk_flow_box_new();
	GSList *group = NULL;

	heading(g, 0, _("Theme"));
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
		gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_(themes[i].label)), FALSE, FALSE, 0);
		gtk_container_add(GTK_CONTAINER(b), box);
		g_signal_connect(b, "toggled", G_CALLBACK(on_theme), (gpointer)themes[i].name);
		gtk_container_add(GTK_CONTAINER(flow), b);
		theme_buttons[i] = b;
	}
	gtk_grid_attach(GTK_GRID(g), flow, 0, 1, 2, 1);

	heading(g, 2, _("Accent color"));
	accent_switch = gtk_switch_new();
	g_signal_connect(accent_switch, "notify::active", G_CALLBACK(on_accent_switch), NULL);
	row(g, 3, _("Own accent color"), accent_switch);
	accent_button = gtk_color_button_new();
	g_signal_connect(accent_button, "color-set", G_CALLBACK(on_accent), NULL);
	row(g, 4, _("Color"), accent_button);
}

static void build_wallpaper(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "wallpaper", _("Wallpaper"));
	GtkFileFilter *images = gtk_file_filter_new();

	heading(g, 0, _("Wallpaper"));
	wall_radio[0] = gtk_radio_button_new_with_label(NULL, _("Matching the theme"));
	wall_radio[1] = gtk_radio_button_new_with_label_from_widget(
		GTK_RADIO_BUTTON(wall_radio[0]), _("Plain color"));
	wall_radio[2] = gtk_radio_button_new_with_label_from_widget(
		GTK_RADIO_BUTTON(wall_radio[0]), _("Picture:"));
	for (int i = 0; i < 3; i++) {
		g_signal_connect(wall_radio[i], "toggled", G_CALLBACK(on_wallpaper), NULL);
		gtk_grid_attach(GTK_GRID(g), wall_radio[i], 0, 1 + i, 1, 1);
	}
	wall_file = gtk_file_chooser_button_new(_("Choose a picture"),
	                                        GTK_FILE_CHOOSER_ACTION_OPEN);
	gtk_file_filter_set_name(images, _("Pictures"));
	gtk_file_filter_add_mime_type(images, "image/*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(wall_file), images);
	gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(wall_file),
	                                    "/usr/share/backgrounds");
	g_signal_connect(wall_file, "file-set", G_CALLBACK(on_wallpaper_file), NULL);
	gtk_grid_attach(GTK_GRID(g), wall_file, 1, 3, 1, 1);

	wall_mode = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "crop", _("Fill the screen"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "fit", _("Fit, keep all of it"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "stretch", _("Stretch"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "center", _("Centre"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(wall_mode), "tile", _("Tile"));
	g_signal_connect(wall_mode, "changed", G_CALLBACK(on_combo_text), cfg.wallpaper_mode);
	row(g, 4, _("Size"), wall_mode);

	wall_preview = gtk_image_new();
	gtk_widget_set_halign(wall_preview, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(g), wall_preview, 0, 5, 2, 1);
}

static void build_clock(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "clock", _("Clock"));

	heading(g, 0, _("Style"));
	row(g, 1, _("Taskbar clock"), bind(new_choice(&cfg.panel_clock, _("Digital"), _("Analog"))));
	row(g, 2, _("Clock app"), bind(new_choice(&cfg.clock_app, _("Digital"), _("Analog"))));
	heading(g, 3, _("Format"));
	row(g, 4, _("24-hour time"), bind(new_switch(&cfg.clock_24h)));
	row(g, 5, _("Show seconds"), bind(new_switch(&cfg.clock_seconds)));
	row(g, 6, _("Show the date"), bind(new_switch(&cfg.clock_date)));
	date_combo = gtk_combo_box_text_new();
	g_signal_connect(date_combo, "changed", G_CALLBACK(on_combo_text), cfg.date_format);
	row(g, 7, _("Date format"), date_combo);
}

static void build_taskbar(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "taskbar", _("Taskbar"));

	heading(g, 0, _("Taskbar"));
	row(g, 1, _("Position"), bind(new_choice(&cfg.panel_position, _("Bottom"), _("Top"))));
	row(g, 2, _("Height (pixels)"), bind(new_spin(&cfg.panel_height, 28, 56)));
	row(g, 3, _("Text size"), bind(new_spin(&cfg.font_size, 8, 16)));
}

/* --- Display: screens with xrandr ------------------------------------------ */

#define MAX_OUTPUTS 4
#define MAX_MODES   48

/* One screen output as `xrandr --query` lists it:
 *   HDMI-1 connected primary 1920x1080+0+0 (normal left ...) 527mm x 296mm
 *      1920x1080     60.00*+  50.00
 *      1280x720      60.00
 */
struct output {
	char name[32];
	int primary, on;              /* on: shows a picture now */
	char mode[32];                /* its resolution now ("1920x1080") */
	int x, y;
	char modes[MAX_MODES][32];
	int nmodes;
};

static struct output outputs[MAX_OUTPUTS];
static int noutputs;
static GtkWidget *mode_combo[MAX_OUTPUTS], *arrange_combo, *scale_combo, *lock_combo;
static GtkWidget *display_status;

static void read_outputs(void)
{
	char *out = NULL, **lines;
	struct output *cur = NULL;

	noutputs = 0;
	if (!g_spawn_command_line_sync("xrandr --query", &out, NULL, NULL, NULL) || !out)
		return;
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *l = lines[i], name[32], geo[64];
		int w, h, x, y;

		if (l[0] != ' ' && l[0] != '\t') {
			cur = NULL;
			if (strstr(l, " connected") && noutputs < MAX_OUTPUTS &&
			    sscanf(l, "%31s", name) == 1) {
				cur = &outputs[noutputs++];
				memset(cur, 0, sizeof *cur);
				snprintf(cur->name, sizeof cur->name, "%s", name);
				cur->primary = strstr(l, " primary ") != NULL;
				/* the first WxH+X+Y after the name: on, with that mode */
				for (char *p = strchr(l, ' '); p && *p; p = strchr(p + 1, ' '))
					if (sscanf(p, " %63s", geo) == 1 &&
					    sscanf(geo, "%dx%d+%d+%d", &w, &h, &x, &y) == 4) {
						cur->on = 1;
						snprintf(cur->mode, sizeof cur->mode, "%dx%d", w, h);
						cur->x = x;
						cur->y = y;
						break;
					}
			}
		} else if (cur && cur->nmodes < MAX_MODES && sscanf(l, " %31s", name) == 1 &&
		           strchr(name, 'x')) {
			int dup = 0;

			for (int m = 0; m < cur->nmodes; m++)
				dup |= strcmp(cur->modes[m], name) == 0;
			if (!dup)
				snprintf(cur->modes[cur->nmodes++], 32, "%s", name);
		}
	}
	g_strfreev(lines);
	g_free(out);
	/* the main screen first */
	for (int i = 1; i < noutputs; i++)
		if (outputs[i].primary) {
			struct output t = outputs[0];

			outputs[0] = outputs[i];
			outputs[i] = t;
		}
}

/* The xrandr options that bring back what is on the screens now */
static char *current_layout(void)
{
	GString *s = g_string_new("");

	for (int i = 0; i < noutputs; i++) {
		if (outputs[i].on)
			g_string_append_printf(s, "--output %s --mode %s --pos %dx%d%s ",
				outputs[i].name, outputs[i].mode, outputs[i].x, outputs[i].y,
				i == 0 ? " --primary" : "");
		else
			g_string_append_printf(s, "--output %s --off ", outputs[i].name);
	}
	return g_string_free(s, FALSE);
}

/* The xrandr options for what is chosen on the page */
static char *chosen_layout(void)
{
	const char *m0 = gtk_combo_box_get_active_id(GTK_COMBO_BOX(mode_combo[0]));
	const char *m1 = noutputs > 1 ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(mode_combo[1])) : NULL;
	const char *how = noutputs > 1 ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(arrange_combo)) : "";
	const char *a = outputs[0].name, *b = noutputs > 1 ? outputs[1].name : "";
	GString *s = g_string_new("");

	if (!m0)
		m0 = outputs[0].mode;
	if (noutputs < 2 || !m1) {
		g_string_printf(s, "--output %s --mode %s --primary", a, m0);
	} else if (strcmp(how, "only-main") == 0) {
		g_string_printf(s, "--output %s --mode %s --primary --output %s --off", a, m0, b);
	} else if (strcmp(how, "only-second") == 0) {
		g_string_printf(s, "--output %s --off --output %s --mode %s --primary", a, b, m1);
	} else if (strcmp(how, "mirror") == 0) {
		g_string_printf(s, "--output %s --mode %s --primary --output %s --mode %s --same-as %s",
		                a, m0, b, m1, a);
	} else {                                   /* right-of, left-of, above, below */
		g_string_printf(s, "--output %s --mode %s --primary --pos 0x0 --output %s --mode %s --%s %s",
		                a, m0, b, m1, how, a);
	}
	/* the others (a third screen) are left as they are */
	return g_string_free(s, FALSE);
}

static void fill_display_page(void)
{
	for (int i = 0; i < 2; i++) {
		GtkComboBoxText *c = GTK_COMBO_BOX_TEXT(mode_combo[i]);

		gtk_combo_box_text_remove_all(c);
		gtk_widget_set_visible(mode_combo[i], i < noutputs);
		gtk_widget_set_visible(g_object_get_data(G_OBJECT(mode_combo[i]), "label"), i < noutputs);
		if (i >= noutputs)
			continue;
		for (int m = 0; m < outputs[i].nmodes; m++)
			gtk_combo_box_text_append(c, outputs[i].modes[m], outputs[i].modes[m]);
		if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), outputs[i].mode))
			gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);    /* off: its best mode */
		gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(mode_combo[i]), "label")),
			i == 0 ? (noutputs > 1 ? _("Main screen") : _("Resolution")) : _("Second screen"));
		gtk_widget_set_tooltip_text(mode_combo[i], outputs[i].name);
	}
	gtk_widget_set_visible(arrange_combo, noutputs > 1);
	gtk_widget_set_visible(g_object_get_data(G_OBJECT(arrange_combo), "label"), noutputs > 1);
	if (noutputs > 1) {
		const char *how = "right-of";

		if (!outputs[1].on)
			how = "only-main";
		else if (!outputs[0].on)
			how = "only-second";
		else if (outputs[1].x == outputs[0].x && outputs[1].y == outputs[0].y)
			how = "mirror";
		else if (outputs[1].x < outputs[0].x)
			how = "left-of";
		else if (outputs[1].y > outputs[0].y && outputs[1].x == outputs[0].x)
			how = "below";
		else if (outputs[1].y < outputs[0].y && outputs[1].x == outputs[0].x)
			how = "above";
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(arrange_combo), how);
	}
}

/* "Keep these settings?" with a countdown: a screen that shows nothing can't
 * be clicked, so without an answer the old settings come back. */
static int keep_seconds;

static gboolean countdown(gpointer dlg)
{
	char *t;

	if (--keep_seconds <= 0) {
		gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_REJECT);
		return G_SOURCE_REMOVE;
	}
	t = g_strdup_printf(_("Going back to the old settings in %d seconds."), keep_seconds);
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", t);
	g_free(t);
	return G_SOURCE_CONTINUE;
}

static void on_display_apply(GtkButton *b, gpointer d)
{
	char *before = current_layout(), *after = chosen_layout(), path[512], *dir;
	GtkWidget *dlg, *win = gtk_widget_get_toplevel(GTK_WIDGET(b));
	guint timer;
	int answer;

	(void)d;
	if (!run_xrandr(after)) {
		gtk_label_set_text(GTK_LABEL(display_status), _("The screen can't show that."));
		run_xrandr(before);
		goto out;
	}
	dlg = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
	                             GTK_BUTTONS_NONE, _("Keep these display settings?"));
	gtk_dialog_add_buttons(GTK_DIALOG(dlg), _("Go back"), GTK_RESPONSE_REJECT,
	                       _("Keep"), GTK_RESPONSE_ACCEPT, NULL);
	keep_seconds = 16;
	countdown(dlg);
	timer = g_timeout_add_seconds(1, countdown, dlg);
	answer = gtk_dialog_run(GTK_DIALOG(dlg));
	if (keep_seconds > 0)
		g_source_remove(timer);
	gtk_widget_destroy(dlg);

	if (answer == GTK_RESPONSE_ACCEPT) {
		FILE *f;

		display_file(path, sizeof path);
		dir = g_path_get_dirname(path);
		make_dirs(dir);
		g_free(dir);
		if ((f = fopen(path, "w"))) {
			fprintf(f, "%s\n", after);
			fclose(f);
		}
		gtk_label_set_text(GTK_LABEL(display_status), _("Saved: also at the next login."));
		apply_desktop(&cfg);            /* the wallpaper for the new size */
	} else {
		run_xrandr(before);
		gtk_label_set_text(GTK_LABEL(display_status), _("The old settings are back."));
	}
out:
	g_free(before);
	g_free(after);
	read_outputs();
	fill_display_page();
}

static void on_display_reset(GtkButton *b, gpointer d)
{
	char path[512], *opts = g_strdup_printf("--output %s --auto --primary", outputs[0].name);
	GString *s = g_string_new(opts);

	(void)b; (void)d;
	for (int i = 1; i < noutputs; i++)          /* others: right of the main one */
		g_string_append_printf(s, " --output %s --auto --right-of %s",
		                       outputs[i].name, outputs[i - 1].name);
	if (noutputs > 0)
		run_xrandr(s->str);
	display_file(path, sizeof path);
	unlink(path);
	gtk_label_set_text(GTK_LABEL(display_status), _("Back to what the screens prefer."));
	g_string_free(s, TRUE);
	g_free(opts);
	read_outputs();
	fill_display_page();
	apply_desktop(&cfg);
}

/* A drop-down list whose item ids are numbers, bound to an int of cfg */
static void on_combo_number(GtkComboBox *c, gpointer field)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (building || !id)
		return;
	*(int *)field = atoi(id);
	changed();
}

static GtkWidget *number_choice(int *field, const char *const items[][2])
{
	GtkWidget *c = gtk_combo_box_text_new();

	for (int i = 0; items[i][0]; i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), items[i][0], _(items[i][1]));
	g_signal_connect(c, "changed", G_CALLBACK(on_combo_number), field);
	return c;
}

static void labeled_row(GtkWidget *g, int y, const char *text, GtkWidget *w)
{
	GtkWidget *l = gtk_label_new(text);

	gtk_widget_set_halign(l, GTK_ALIGN_START);
	gtk_widget_set_halign(w, GTK_ALIGN_START);
	gtk_grid_attach(GTK_GRID(g), l, 0, y, 1, 1);
	gtk_grid_attach(GTK_GRID(g), w, 1, y, 1, 1);
	g_object_set_data(G_OBJECT(w), "label", l);
}

static void build_display(GtkWidget *stack)
{
	static const char *const scales[][2] = {
		{ "100", "100 %" }, { "125", "125 %" }, { "150", "150 %" },
		{ "175", "175 %" }, { "200", N_("200 % (for very sharp screens)") }, { NULL, NULL } };
	GtkWidget *g = page(stack, "display", _("Display"));
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *apply, *reset;

	heading(g, 0, _("Screens"));
	for (int i = 0; i < 2; i++) {
		mode_combo[i] = gtk_combo_box_text_new();
		labeled_row(g, 1 + i, i == 0 ? _("Resolution") : _("Second screen"), mode_combo[i]);
	}
	arrange_combo = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(arrange_combo), "right-of", _("Right of the main screen"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(arrange_combo), "left-of", _("Left of the main screen"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(arrange_combo), "above", _("Above the main screen"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(arrange_combo), "below", _("Below the main screen"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(arrange_combo), "mirror", _("Same picture on both"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(arrange_combo), "only-main", _("Only the main screen"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(arrange_combo), "only-second", _("Only the second screen"));
	labeled_row(g, 3, _("Second screen is"), arrange_combo);

	apply = gtk_button_new_with_label(_("Apply"));
	gtk_style_context_add_class(gtk_widget_get_style_context(apply), "suggested-action");
	g_signal_connect(apply, "clicked", G_CALLBACK(on_display_apply), NULL);
	reset = gtk_button_new_with_label(_("Automatic"));
	g_signal_connect(reset, "clicked", G_CALLBACK(on_display_reset), NULL);
	gtk_box_pack_start(GTK_BOX(buttons), apply, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(buttons), reset, FALSE, FALSE, 0);
	gtk_grid_attach(GTK_GRID(g), buttons, 1, 4, 1, 1);
	display_status = gtk_label_new("");
	gtk_widget_set_halign(display_status, GTK_ALIGN_START);
	gtk_style_context_add_class(gtk_widget_get_style_context(display_status), "dim-label");
	gtk_grid_attach(GTK_GRID(g), display_status, 1, 5, 1, 1);

	heading(g, 6, _("Size"));
	scale_combo = number_choice(&cfg.scale, scales);
	row(g, 7, _("Text and icons"), scale_combo);
	{
		GtkWidget *l = gtk_label_new(_("Programs you open from now on use the new size; "
		                             "all of the desktop after the next login."));

		gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
		gtk_grid_attach(GTK_GRID(g), l, 1, 8, 1, 1);
	}

	read_outputs();
}

static void on_lock_now(GtkButton *b, gpointer d)
{
	(void)b; (void)d;
	g_spawn_command_line_async("my-lock", NULL);
}

static void build_lock(GtkWidget *stack)
{
	static const char *const times[][2] = {
		{ "1", N_("1 minute") }, { "2", N_("2 minutes") }, { "5", N_("5 minutes") },
		{ "10", N_("10 minutes") }, { "15", N_("15 minutes") }, { "30", N_("30 minutes") },
		{ "60", N_("1 hour") }, { "0", N_("Never") }, { NULL, NULL } };
	GtkWidget *g = page(stack, "lock", _("Screen lock"));
	GtkWidget *now = gtk_button_new_with_label(_("Lock now"));
	GtkWidget *l = gtk_label_new(_("The lock screen asks for your password. Lock it yourself "
	                             "with Super+L or Start menu, Lock screen. It also locks "
	                             "before the computer goes to sleep."));

	heading(g, 0, _("Screen lock"));
	lock_combo = number_choice(&cfg.lock_minutes, times);
	row(g, 1, _("Lock after no use for"), lock_combo);
	g_signal_connect(now, "clicked", G_CALLBACK(on_lock_now), NULL);
	row(g, 2, "", now);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 3, 2, 1);
}

/* --- Accessibility -------------------------------------------------------------- */

static GtkWidget *contrast_switch, *a11y_scale_combo, *cursor_combo;

/* High contrast on: the matching high-contrast theme (dark, or light for
 * someone who had the light theme). Off: the system's default theme. */
static void on_contrast(GObject *o, GParamSpec *ps, gpointer d)
{
	int on = gtk_switch_get_active(GTK_SWITCH(o));

	(void)ps; (void)d;
	if (building || on == settings_high_contrast(&cfg))
		return;
	if (on) {
		g_strlcpy(cfg.theme, cfg.pal.dark ? "contrast" : "contrast-light", sizeof cfg.theme);
	} else {
		struct settings def;

		settings_load(&def, "/nonexistent");     /* /etc/my-distro/settings only */
		g_strlcpy(cfg.theme, def.theme, sizeof cfg.theme);
	}
	cfg.accent[0] = '\0';
	changed();
}

/* The pointer: X programs read its size when they start; Openbox (window
 * borders) is restarted for it, which keeps all windows open. */
static void on_cursor(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (building || !id || atoi(id) == cfg.cursor_size)
		return;
	cfg.cursor_size = atoi(id);
	changed();
	g_spawn_command_line_async("openbox --restart", NULL);
}

static void on_run(GtkButton *b, gpointer cmd)
{
	(void)b;
	g_spawn_command_line_async(cmd, NULL);
}

/* The screen reader (0.9.7): on now and at every login, or off */
static GtkWidget *reader_switch;

static void on_reader(GObject *sw, GParamSpec *ps, gpointer d)
{
	int on = gtk_switch_get_active(GTK_SWITCH(sw));

	(void)ps; (void)d;
	if (building || on == cfg.screen_reader)
		return;
	cfg.screen_reader = on;
	changed();
	g_spawn_command_line_async(on ? "my-reader --autostart" : "my-reader --stop", NULL);
}

static void build_access(GtkWidget *stack)
{
	static const char *const scales[][2] = {
		{ "100", N_("Normal") }, { "125", "125 %" }, { "150", "150 %" },
		{ "175", "175 %" }, { "200", "200 %" }, { NULL, NULL } };
	static const char *const cursors[][2] = {
		{ "24", N_("Normal") }, { "32", N_("Bigger") }, { "48", N_("Large") },
		{ "64", N_("Very large") }, { "96", N_("Huge") }, { NULL, NULL } };
	GtkWidget *g = page(stack, "access", _("Accessibility"));
	GtkWidget *b, *l;
	int y = 0;

	heading(g, y++, _("Seeing"));
	contrast_switch = gtk_switch_new();
	g_signal_connect(contrast_switch, "notify::active", G_CALLBACK(on_contrast), NULL);
	row(g, y++, _("High contrast"), contrast_switch);
	a11y_scale_combo = number_choice(&cfg.scale, scales);
	row(g, y++, _("Size of text and icons"), a11y_scale_combo);
	cursor_combo = gtk_combo_box_text_new();
	for (int i = 0; cursors[i][0]; i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cursor_combo), cursors[i][0], _(cursors[i][1]));
	g_signal_connect(cursor_combo, "changed", G_CALLBACK(on_cursor), NULL);
	row(g, y++, _("Mouse pointer"), cursor_combo);
	b = gtk_button_new_with_label(_("Open the magnifier"));
	g_signal_connect(b, "clicked", G_CALLBACK(on_run), "my-zoom");
	row(g, y++, _("Magnifier (Super+Z)"), b);
	b = gtk_button_new_with_label(_("Read the selected text"));
	g_signal_connect(b, "clicked", G_CALLBACK(on_run), "my-speak");
	row(g, y++, _("Read aloud (Super+R)"), b);
	reader_switch = gtk_switch_new();
	gtk_widget_set_halign(reader_switch, GTK_ALIGN_START);
	g_signal_connect(reader_switch, "notify::active", G_CALLBACK(on_reader), NULL);
	row(g, y++, _("Screen reader (Super+Alt+S)"), reader_switch);

	heading(g, y++, _("Typing"));
	b = gtk_button_new_with_label(_("Open the on-screen keyboard"));
	g_signal_connect(b, "clicked", G_CALLBACK(on_run), "my-keyboard");
	row(g, y++, _("On-screen keyboard (Super+K)"), b);

	l = gtk_label_new(_("Text size applies to programs you open from now on, all of the "
		"desktop after the next login. The on-screen keyboard is also on the login "
		"screen (the keyboard button). Read aloud: select text with the mouse in any "
		"program, then press Super+R; again stops it. The screen reader speaks window "
		"titles, the Start menu and what has the focus in GTK programs (buttons, fields, "
		"lists, menus); not terminals, web pages or the login screen."));
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, y, 2, 1);
}

/* --- Sound (0.9.7): the Sound window has it all ------------------------------- */

static void build_sound(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "sound", _("Sound"));
	GtkWidget *b = gtk_button_new_with_label(_("Open the Sound window"));
	GtkWidget *l = gtk_label_new(_("Where the sound plays (speakers, headphones, HDMI, "
		"Bluetooth), the microphone, and a volume for every program that plays: the Sound "
		"window. The speaker in the taskbar: scroll for louder or quieter, click to mute, "
		"right-click for the Sound window."));

	heading(g, 0, _("Sound"));
	g_signal_connect(b, "clicked", G_CALLBACK(on_run), "my-sound");
	row(g, 1, _("Outputs and programs"), b);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 2, 2, 1);
}

/* --- Permissions (0.9.7) ---------------------------------------------------- */
/*
 * Per program: may it use the camera, the microphone, and how much of the
 * home folder may it see. Saved in ~/.config/my-distro/permissions; my-run
 * applies it (with a sandbox) when the Start menu starts the program. A
 * program with every permission has no line there and starts as it always
 * did. The list: the Start menu's programs (system tools left out: they
 * need the system).
 */
#define PERM_FILE ".config/my-distro/permissions"

struct perm {
	char prog[64], label[64];
	int camera, mic;
	char home[16];                 /* full, read, downloads, none */
};
static struct perm perms[64];
static int nperms;

static void perm_save(void)
{
	char *path = g_build_filename(g_get_home_dir(), PERM_FILE, NULL), *dir;
	GString *s = g_string_new("# Permissions of programs started from the Start menu "
		"(Settings, Permissions; my-run applies them).\n# PROGRAM camera=yes|no "
		"microphone=yes|no home=full|read|downloads|none\n");

	for (int i = 0; i < nperms; i++)
		if (!perms[i].camera || !perms[i].mic || strcmp(perms[i].home, "full"))
			g_string_append_printf(s, "%s camera=%s microphone=%s home=%s\n", perms[i].prog,
			                       perms[i].camera ? "yes" : "no", perms[i].mic ? "yes" : "no",
			                       perms[i].home);
	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0755);
	g_file_set_contents(path, s->str, -1, NULL);
	g_free(dir);
	g_free(path);
	g_string_free(s, TRUE);
}

/* The menu files' programs, with what the permissions file says */
static void perm_load(void)
{
	const char *menus[] = { "/etc/my-panel/menu", "/var/lib/my-pkg/menu" };
	char *path = g_build_filename(g_get_home_dir(), PERM_FILE, NULL), *text = NULL;

	nperms = 0;
	for (size_t m = 0; m < G_N_ELEMENTS(menus); m++) {
		char **lines;

		if (!g_file_get_contents(menus[m], &text, NULL, NULL))
			continue;
		lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i] && nperms < (int)G_N_ELEMENTS(perms); i++) {
			char **f = g_strsplit(lines[i], "|", 3), *prog;
			int dup = 0;

			if (g_strv_length(f) < 2 || lines[i][0] == '#') {
				g_strfreev(f);
				continue;
			}
			g_strstrip(f[1]);
			f[1][strcspn(f[1], " ")] = '\0';     /* the command's first word */
			prog = g_path_get_basename(f[1]);
			for (int j = 0; j < nperms; j++)
				dup |= !strcmp(perms[j].prog, prog);
			/* my-distro's own tools, terminals and the session's controls */
			if (!dup && strncmp(prog, "my-", 3) && strcmp(prog, "xterm") &&
			    strcmp(prog, "openbox") && strcmp(prog, "pcmanfm")) {
				struct perm *p = &perms[nperms++];

				g_strlcpy(p->prog, prog, sizeof p->prog);
				g_strlcpy(p->label, _(f[0]), sizeof p->label);
				p->camera = p->mic = 1;
				g_strlcpy(p->home, "full", sizeof p->home);
			}
			g_free(prog);
			g_strfreev(f);
		}
		g_strfreev(lines);
		g_free(text);
	}
	if (g_file_get_contents(path, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", -1);

		for (int i = 0; lines[i]; i++) {
			char prog[64], cam[8] = "yes", mic[8] = "yes", home[16] = "full";

			if (lines[i][0] == '#' || sscanf(lines[i], "%63s camera=%7s microphone=%7s home=%15s",
			                                 prog, cam, mic, home) < 1)
				continue;
			for (int j = 0; j < nperms; j++)
				if (!strcmp(perms[j].prog, prog)) {
					perms[j].camera = strcmp(cam, "no") != 0;
					perms[j].mic = strcmp(mic, "no") != 0;
					g_strlcpy(perms[j].home, home, sizeof perms[j].home);
				}
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(path);
}

static void on_perm_check(GtkToggleButton *b, gpointer field)
{
	*(int *)field = gtk_toggle_button_get_active(b);
	perm_save();
}

static void on_perm_home(GtkComboBox *c, gpointer d)
{
	struct perm *p = d;
	const char *id = gtk_combo_box_get_active_id(c);

	if (id) {
		g_strlcpy(p->home, id, sizeof p->home);
		perm_save();
	}
}

static void build_permissions(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "permissions", _("Permissions"));
	GtkWidget *l = gtk_label_new(_("What each program may use, the next time you start it from "
		"the Start menu. A program without the microphone gets no sound at all (the sound "
		"server can't give it one without the other). Without the whole home folder it sees "
		"only Downloads (or nothing) and keeps its own settings separately."));
	int y = 1;

	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 0, 4, 1);
	gtk_grid_set_row_spacing(GTK_GRID(g), 8);
	gtk_grid_attach(GTK_GRID(g), gtk_label_new(_("Camera")), 1, y, 1, 1);
	gtk_grid_attach(GTK_GRID(g), gtk_label_new(_("Microphone")), 2, y, 1, 1);
	gtk_grid_attach(GTK_GRID(g), gtk_label_new(_("Home folder")), 3, y++, 1, 1);
	perm_load();
	for (int i = 0; i < nperms; i++, y++) {
		struct perm *p = &perms[i];
		GtkWidget *name = gtk_label_new(p->label), *cam = gtk_check_button_new(),
		          *mic = gtk_check_button_new(), *home = gtk_combo_box_text_new();

		gtk_widget_set_halign(name, GTK_ALIGN_START);
		gtk_widget_set_halign(cam, GTK_ALIGN_CENTER);
		gtk_widget_set_halign(mic, GTK_ALIGN_CENTER);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cam), p->camera);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mic), p->mic);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(home), "full", _("All of it"));
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(home), "read", _("Read only"));
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(home), "downloads", _("Only Downloads"));
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(home), "none", _("None"));
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(home), p->home);
		g_signal_connect(cam, "toggled", G_CALLBACK(on_perm_check), &p->camera);
		g_signal_connect(mic, "toggled", G_CALLBACK(on_perm_check), &p->mic);
		g_signal_connect(home, "changed", G_CALLBACK(on_perm_home), p);
		gtk_grid_attach(GTK_GRID(g), name, 0, y, 1, 1);
		gtk_grid_attach(GTK_GRID(g), cam, 1, y, 1, 1);
		gtk_grid_attach(GTK_GRID(g), mic, 2, y, 1, 1);
		gtk_grid_attach(GTK_GRID(g), home, 3, y, 1, 1);
	}
}

/* --- Language ------------------------------------------------------------- */

static GtkWidget *lang_combo, *proxy_entry, *noproxy_entry, *proxy_status;

static void on_language(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (!id)
		return;
	g_strlcpy(cfg.language, strcmp(id, "system") == 0 ? "" : id, sizeof cfg.language);
	changed();
}

static void build_language(GtkWidget *stack)
{
	static const struct { const char *id, *label; } langs[] = { LANGUAGES };
	GtkWidget *g = page(stack, "language", _("Language"));
	GtkWidget *l = gtk_label_new(_("The language of the menus and windows, and the format "
		"of dates and numbers. It applies from the next login. Programs that "
		"aren't translated stay in English."));

	heading(g, 0, _("Language"));
	lang_combo = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang_combo), "system",
	                          _("The system's language (Users and system)"));
	for (size_t i = 0; i < sizeof langs / sizeof langs[0]; i++)
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang_combo), langs[i].id, langs[i].label);
	g_signal_connect(lang_combo, "changed", G_CALLBACK(on_language), NULL);
	row(g, 1, _("Language"), lang_combo);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 2, 2, 1);
}

/* --- Network: proxy -------------------------------------------------------- */
/*
 * The proxy goes into the settings file; /usr/lib/my-distro/session-env
 * turns it into http_proxy & co for the whole session at login (my-update,
 * pip, wget, git read those). NetSurf has its own settings
 * (~/.config/netsurf/Choices): we write the proxy there too.
 */
static void netsurf_proxy(const char *host, int port)
{
	char *path = g_build_filename(g_get_home_dir(), ".config", "netsurf", "Choices", NULL);
	char *text = NULL, **lines;
	GString *out = g_string_new(NULL);

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		lines = g_strsplit(text, "\n", -1);
		for (int i = 0; lines[i]; i++)
			if (lines[i][0] && !g_str_has_prefix(lines[i], "http_proxy"))
				g_string_append_printf(out, "%s\n", lines[i]);
		g_strfreev(lines);
		g_free(text);
	}
	if (host && host[0])
		g_string_append_printf(out, "http_proxy:1\nhttp_proxy_host:%s\n"
		                       "http_proxy_port:%d\nhttp_proxy_auth:0\n", host, port);
	else
		g_string_append(out, "http_proxy:0\n");
	{
		char *dir = g_path_get_dirname(path);

		make_dirs(dir);
		g_free(dir);
	}
	g_file_set_contents(path, out->str, -1, NULL);
	g_string_free(out, TRUE);
	g_free(path);
}

static void on_proxy_save(GtkButton *b, gpointer d)
{
	char *p = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(proxy_entry))));
	char *np = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(noproxy_entry))));
	char host[160] = "", *url;
	int port = 8080;

	(void)b; (void)d;
	if (strpbrk(p, " \t\"'") || strpbrk(np, " \t\"'")) {
		gtk_label_set_text(GTK_LABEL(proxy_status), _("No spaces or quotes, please."));
		goto out;
	}
	/* "proxy:3128" -> "http://proxy:3128" */
	url = p[0] && !strstr(p, "://") ? g_strdup_printf("http://%s", p) : g_strdup(p);
	if (url[0] && sscanf(strstr(url, "://") + 3, "%159[^:/]:%d", host, &port) < 1) {
		gtk_label_set_text(GTK_LABEL(proxy_status), _("Write the proxy as NAME:PORT, "
		                   "e.g. proxy.example.com:3128."));
		g_free(url);
		goto out;
	}
	g_strlcpy(cfg.proxy, url, sizeof cfg.proxy);
	g_strlcpy(cfg.no_proxy, np, sizeof cfg.no_proxy);
	changed();
	netsurf_proxy(host, port);
	gtk_label_set_text(GTK_LABEL(proxy_status), url[0]
		? _("Saved. Log out and in again so that all programs use it.")
		: _("No proxy. Log out and in again so that all programs know."));
	g_free(url);
out:
	g_free(p);
	g_free(np);
}

static void build_network(GtkWidget *stack)
{
	GtkWidget *g = page(stack, "network", _("Network"));
	GtkWidget *save = gtk_button_new_with_label(_("Save"));
	GtkWidget *l = gtk_label_new(_("Some networks (schools, companies) only reach the "
		"internet through a proxy. It is used by the web browser, system updates and "
		"other programs that download. Leave it empty for none."));

	heading(g, 0, _("Proxy"));
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 1, 2, 1);
	proxy_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(proxy_entry), "proxy.example.com:3128");
	row(g, 2, _("HTTP and HTTPS proxy"), proxy_entry);
	noproxy_entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(noproxy_entry), "localhost,127.0.0.1,.local");
	row(g, 3, _("Without proxy for"), noproxy_entry);
	g_signal_connect(save, "clicked", G_CALLBACK(on_proxy_save), NULL);
	row(g, 4, "", save);
	proxy_status = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(proxy_status), 0);
	gtk_label_set_line_wrap(GTK_LABEL(proxy_status), TRUE);
	gtk_grid_attach(GTK_GRID(g), proxy_status, 0, 5, 2, 1);
	heading(g, 6, _("Sharing"));
	l = gtk_label_new(_("The firewall lets nobody in, unless you share a folder or the "
		"printers with the other computers of your network (Sharing). To open a shared "
		"folder of another PC: Start menu, Network share."));
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(l), "dim-label");
	gtk_grid_attach(GTK_GRID(g), l, 0, 7, 2, 1);
	save = gtk_button_new_with_label(_("Sharing…"));
	g_signal_connect(save, "clicked", G_CALLBACK(on_run), "my-sharing");
	row(g, 8, "", save);
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

	/* screen lock and size: numbers as the items' ids */
	snprintf(example, sizeof example, "%d", cfg.lock_minutes);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(lock_combo), example)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lock_combo), example, example);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(lock_combo), example);
	}
	snprintf(example, sizeof example, "%d", cfg.scale);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(scale_combo), example)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(scale_combo), example, example);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(scale_combo), example);
	}
	/* Accessibility: the same size, high contrast, pointer */
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(a11y_scale_combo), example)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a11y_scale_combo), example, example);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(a11y_scale_combo), example);
	}
	gtk_switch_set_active(GTK_SWITCH(contrast_switch), settings_high_contrast(&cfg));
	gtk_switch_set_active(GTK_SWITCH(reader_switch), cfg.screen_reader);
	snprintf(example, sizeof example, "%d", cfg.cursor_size);
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(cursor_combo), example)) {
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(cursor_combo), example, example);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(cursor_combo), example);
	}

	/* language and proxy */
	if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(lang_combo),
	                                 cfg.language[0] ? cfg.language : "system"))
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(lang_combo), "system");
	gtk_entry_set_text(GTK_ENTRY(proxy_entry), cfg.proxy);
	gtk_entry_set_text(GTK_ENTRY(noproxy_entry), cfg.no_proxy);

	building = 0;
}

int main(int argc, char **argv)
{
	GtkWidget *win, *box, *side, *stack, *vbox, *bottom, *reset, *users;

	if (argc > 1 && strcmp(argv[1], "--apply") == 0) {
		/* at login: no window, so GTK is only needed for the pictures */
		settings_load(&cfg, NULL);
		apply_display();          /* first: the wallpaper fits the new size */
		apply_desktop(&cfg);
		return 0;
	}

	i18n_init();
	gtk_init(&argc, &argv);
	settings_load(&cfg, NULL);

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), _("Settings"));
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
	build_display(stack);
	build_lock(stack);
	build_sound(stack);
	build_access(stack);
	build_permissions(stack);
	build_language(stack);
	build_network(stack);

	side = gtk_stack_sidebar_new();
	gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
	gtk_widget_set_size_request(side, 150, -1);

	reset = gtk_button_new_with_label(_("Reset to defaults"));
	g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), NULL);
	bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_container_set_border_width(GTK_CONTAINER(bottom), 12);
	gtk_box_pack_end(GTK_BOX(bottom), reset, FALSE, FALSE, 0);
	users = gtk_button_new_with_label(_("Users and system…"));
	g_signal_connect(users, "clicked", G_CALLBACK(on_users), NULL);
	gtk_box_pack_start(GTK_BOX(bottom), users, FALSE, FALSE, 0);

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
	fill_display_page();          /* after show_all: it hides what isn't there */
	if (argc > 2 && strcmp(argv[1], "--page") == 0)
		gtk_stack_set_visible_child_name(GTK_STACK(stack), argv[2]);
	gtk_main();
	return 0;
}
