/*
 * my-sound: the Sound window of my-distro (GTK 3), 0.9.7. Right-click on
 * the taskbar's speaker, or Start menu "Sound".
 *
 *   Output       where the sound goes (speakers, headphones, HDMI, Bluetooth)
 *                and its volume
 *   Microphone   which one records, and how loud
 *   Programs     every program that is playing now: its own volume, and
 *                which output it plays on
 *
 * The sound server PipeWire does it all; /usr/lib/my-distro/sound asks it
 * (pw-dump) and changes it (wpctl, pw-metadata) and prints simple lines
 * (see there). The window reads them every 2 seconds, so programs that
 * start or stop appear and go. WirePlumber remembers the choices.
 */
#include <gtk/gtk.h>

#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELPER "/usr/lib/my-distro/sound"

struct item {
	char kind[4];            /* out, in, app */
	int id, flag, vol, muted;    /* flag: default (out/in) or output id (app) */
	char name[160];
};

static struct item items[64];
static int nitems;
static char shape[2048];         /* the kinds and ids: rebuild when they change */
static GtkWidget *win, *box_out, *box_in, *box_apps, *label_none;
static gint64 user_busy;         /* the user moved a slider: don't overwrite it */
static int filling;              /* setting widgets ourselves: no signals */

static void helper(const char *a, int id, int value)
{
	char sid[16], sval[16];
	const char *argv[] = { HELPER, a, sid, sval, NULL };

	snprintf(sid, sizeof sid, "%d", id);
	snprintf(sval, sizeof sval, "%d", value);
	if (value < 0)
		argv[3] = NULL;
	g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL |
	              G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

static void read_items(void)
{
	const char *argv[] = { HELPER, "list", NULL };
	char *out = NULL, **lines;

	nitems = 0;
	if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
	                  &out, NULL, NULL, NULL) || !out)
		return;
	lines = g_strsplit(out, "\n", -1);
	for (int i = 0; lines[i] && nitems < (int)G_N_ELEMENTS(items); i++) {
		struct item *it = &items[nitems];

		if (sscanf(lines[i], "%3s\t%d\t%d\t%d\t%d\t%159[^\n]", it->kind, &it->id,
		           &it->flag, &it->vol, &it->muted, it->name) == 6)
			nitems++;
	}
	g_strfreev(lines);
	g_free(out);
}

/* --- Changes by the user ------------------------------------------------------------ */

static void on_volume(GtkRange *r, gpointer d)
{
	if (filling)
		return;
	user_busy = g_get_monotonic_time();
	helper("volume", GPOINTER_TO_INT(d), (int)gtk_range_get_value(r));
}

static void on_mute(GtkToggleButton *b, gpointer d)
{
	if (filling)
		return;
	user_busy = g_get_monotonic_time();
	helper("mute", GPOINTER_TO_INT(d), !gtk_toggle_button_get_active(b));
}

/* the combo of outputs or inputs: make the chosen one the default */
static void on_default(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	(void)d;
	if (filling || !id)
		return;
	user_busy = g_get_monotonic_time();
	helper("default", atoi(id), -1);
}

/* a program's combo: play it on that output */
static void on_move(GtkComboBox *c, gpointer d)
{
	const char *id = gtk_combo_box_get_active_id(c);

	if (filling || !id)
		return;
	user_busy = g_get_monotonic_time();
	helper("move", GPOINTER_TO_INT(d), atoi(id));
}

/* --- Building the window --------------------------------------------------------------- */

static void clear(GtkWidget *box)
{
	GList *l = gtk_container_get_children(GTK_CONTAINER(box));

	for (GList *i = l; i; i = i->next)
		gtk_widget_destroy(i->data);
	g_list_free(l);
}

/* A slider with a "sound on" check box after it, for item it */
static GtkWidget *volume_row(struct item *it, int max)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, max, 5);
	GtkWidget *on = gtk_check_button_new_with_label(_("On"));

	gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
	gtk_scale_set_digits(GTK_SCALE(s), 0);
	gtk_range_set_value(GTK_RANGE(s), it->vol);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(on), !it->muted);
	g_signal_connect(s, "value-changed", G_CALLBACK(on_volume), GINT_TO_POINTER(it->id));
	g_signal_connect(on, "toggled", G_CALLBACK(on_mute), GINT_TO_POINTER(it->id));
	g_object_set_data(G_OBJECT(row), "scale", s);
	g_object_set_data(G_OBJECT(row), "on", on);
	g_object_set_data(G_OBJECT(row), "id", GINT_TO_POINTER(it->id));
	gtk_box_pack_start(GTK_BOX(row), s, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(row), on, FALSE, FALSE, 0);
	return row;
}

/* The outputs (kind "out") or inputs ("in"): a list to choose the default,
 * and the volume of the default one */
static void build_devices(GtkWidget *box, const char *kind)
{
	GtkWidget *combo = gtk_combo_box_text_new();
	struct item *dflt = NULL;
	int n = 0;

	for (int i = 0; i < nitems; i++) {
		char id[16];

		if (strcmp(items[i].kind, kind))
			continue;
		snprintf(id, sizeof id, "%d", items[i].id);
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, items[i].name);
		if (!dflt || items[i].flag)
			dflt = &items[i];
		n++;
	}
	if (dflt) {
		char id[16];

		snprintf(id, sizeof id, "%d", dflt->id);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), id);
	}
	if (!n) {
		GtkWidget *l = gtk_label_new(strcmp(kind, "out") ? _("No microphone.") :
		                             _("No sound output: is the sound server running?"));
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
		gtk_widget_destroy(combo);
		return;
	}
	g_signal_connect(combo, "changed", G_CALLBACK(on_default), NULL);
	gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), volume_row(dflt, 100), FALSE, FALSE, 0);
}

static void build_apps(void)
{
	int n = 0;

	for (int i = 0; i < nitems; i++) {
		GtkWidget *row, *l, *combo;
		char id[16];

		if (strcmp(items[i].kind, "app"))
			continue;
		row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		l = gtk_label_new(items[i].name);
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), volume_row(&items[i], 150), FALSE, FALSE, 0);
		combo = gtk_combo_box_text_new();
		for (int j = 0; j < nitems; j++)
			if (!strcmp(items[j].kind, "out")) {
				snprintf(id, sizeof id, "%d", items[j].id);
				gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, items[j].name);
			}
		snprintf(id, sizeof id, "%d", items[i].flag);
		gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), id);
		g_signal_connect(combo, "changed", G_CALLBACK(on_move), GINT_TO_POINTER(items[i].id));
		gtk_box_pack_start(GTK_BOX(row), combo, FALSE, FALSE, 0);
		gtk_widget_set_margin_bottom(row, 10);
		gtk_box_pack_start(GTK_BOX(box_apps), row, FALSE, FALSE, 0);
		n++;
	}
	gtk_widget_set_visible(label_none, n == 0);
}

/* New values for the sliders and check boxes that are there already */
static void update_values(GtkWidget *w)
{
	GtkWidget *s = g_object_get_data(G_OBJECT(w), "scale");

	if (s) {
		int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "id"));

		for (int i = 0; i < nitems; i++)
			if (items[i].id == id) {
				gtk_range_set_value(GTK_RANGE(s), items[i].vol);
				gtk_toggle_button_set_active(
					GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(w), "on")), !items[i].muted);
			}
	}
	if (GTK_IS_CONTAINER(w)) {
		GList *l = gtk_container_get_children(GTK_CONTAINER(w));

		for (GList *i = l; i; i = i->next)
			update_values(i->data);
		g_list_free(l);
	}
}

static gboolean refresh(gpointer d)
{
	char s[sizeof shape] = "";

	(void)d;
	if (g_get_monotonic_time() - user_busy < 1500000)
		return G_SOURCE_CONTINUE;
	read_items();
	for (int i = 0; i < nitems; i++) {
		char one[64];

		snprintf(one, sizeof one, "%s%d:%d ", items[i].kind, items[i].id, items[i].flag);
		g_strlcat(s, one, sizeof s);
	}
	filling = 1;
	if (strcmp(s, shape) != 0) {
		g_strlcpy(shape, s, sizeof shape);
		clear(box_out);
		clear(box_in);
		clear(box_apps);
		build_devices(box_out, "out");
		build_devices(box_in, "in");
		build_apps();
		gtk_widget_show_all(win);
		gtk_widget_set_visible(label_none, strstr(shape, "app") == NULL);
	} else {
		update_values(win);
	}
	filling = 0;
	return G_SOURCE_CONTINUE;
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
	GtkWidget *box, *scroll, *inner;

	i18n_init();
	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), _("Sound"));
	gtk_window_set_default_size(GTK_WINDOW(win), 460, 520);
	gtk_window_set_icon_name(GTK_WINDOW(win), "audio-volume-high");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
	                               GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(win), scroll);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(scroll), box);

	gtk_box_pack_start(GTK_BOX(box), heading(_("Output")), FALSE, FALSE, 0);
	box_out = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start(GTK_BOX(box), box_out, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), heading(_("Microphone")), FALSE, FALSE, 8);
	box_in = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start(GTK_BOX(box), box_in, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), heading(_("Programs")), FALSE, FALSE, 8);
	inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	label_none = gtk_label_new(_("No program is playing sound right now."));
	gtk_label_set_xalign(GTK_LABEL(label_none), 0);
	gtk_style_context_add_class(gtk_widget_get_style_context(label_none), "dim-label");
	gtk_box_pack_start(GTK_BOX(inner), label_none, FALSE, FALSE, 0);
	box_apps = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_pack_start(GTK_BOX(inner), box_apps, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), inner, FALSE, FALSE, 0);

	refresh(NULL);
	gtk_widget_show_all(win);
	gtk_widget_set_visible(label_none, strstr(shape, "app") == NULL);
	g_timeout_add_seconds(2, refresh, NULL);
	gtk_main();
	return 0;
}
