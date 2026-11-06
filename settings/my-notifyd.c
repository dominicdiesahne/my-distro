/*
 * my-notifyd: shows notifications ("Screenshot saved", "USB stick opened",
 * "Battery low") as small boxes in the corner of the screen next to the
 * taskbar.
 *
 * Programs send notifications over the session D-Bus with the standard
 * freedesktop.org interface, org.freedesktop.Notifications: the same one
 * GNOME, KDE and the rest use, so other Linux programs work too. my-notify
 * (and its other name notify-send) sends one from the command line.
 *
 * Each box shows an icon, a bold title and the text. It goes away after a
 * few seconds (critical ones stay until clicked), or when clicked. If the
 * program offered actions (buttons), a click on one is sent back to it.
 * Colors and the side of the screen come from the Settings app (settings.h).
 *
 * The session bus is started by ~/.xinitrc; Openbox's autostart starts us,
 * and D-Bus would also start us when the first notification comes
 * (/usr/share/dbus-1/services/org.freedesktop.Notifications.service).
 */
#include <gtk/gtk.h>
#include <string.h>

#include "settings.h"

#define BOX_W       340
#define GAP         8
#define TIMEOUT_MS  6000          /* when the sender says "default" (-1) */
#define MAX_SHOWN   6

static const char introspection[] =
	"<node>"
	" <interface name='org.freedesktop.Notifications'>"
	"  <method name='Notify'>"
	"   <arg type='s' name='app_name' direction='in'/>"
	"   <arg type='u' name='replaces_id' direction='in'/>"
	"   <arg type='s' name='app_icon' direction='in'/>"
	"   <arg type='s' name='summary' direction='in'/>"
	"   <arg type='s' name='body' direction='in'/>"
	"   <arg type='as' name='actions' direction='in'/>"
	"   <arg type='a{sv}' name='hints' direction='in'/>"
	"   <arg type='i' name='expire_timeout' direction='in'/>"
	"   <arg type='u' name='id' direction='out'/>"
	"  </method>"
	"  <method name='CloseNotification'>"
	"   <arg type='u' name='id' direction='in'/>"
	"  </method>"
	"  <method name='GetCapabilities'>"
	"   <arg type='as' name='capabilities' direction='out'/>"
	"  </method>"
	"  <method name='GetServerInformation'>"
	"   <arg type='s' name='name' direction='out'/>"
	"   <arg type='s' name='vendor' direction='out'/>"
	"   <arg type='s' name='version' direction='out'/>"
	"   <arg type='s' name='spec_version' direction='out'/>"
	"  </method>"
	"  <signal name='NotificationClosed'>"
	"   <arg type='u' name='id'/>"
	"   <arg type='u' name='reason'/>"
	"  </signal>"
	"  <signal name='ActionInvoked'>"
	"   <arg type='u' name='id'/>"
	"   <arg type='s' name='action_key'/>"
	"  </signal>"
	" </interface>"
	"</node>";

/* Why a notification closed (numbers from the specification) */
enum { CLOSED_EXPIRED = 1, CLOSED_DISMISSED = 2, CLOSED_BY_CALL = 3 };

struct note {
	guint id;
	GtkWidget *win;
	guint timer;
	char *default_action;     /* key of the "default" action, or NULL */
};

static GDBusConnection *bus;
static GList *notes;          /* struct note *, newest first */
static guint next_id = 1;
static GtkCssProvider *css;
static struct settings cfg;

/* --- Look ---------------------------------------------------------------------- */

/* The theme's colors as CSS for the boxes */
static void load_style(void)
{
	char *text;

	settings_load(&cfg, NULL);
	text = g_strdup_printf(
		".note { background: %s; border: 1px solid %s; border-left: 4px solid %s;"
		"  border-radius: 6px; }"
		".note.critical { border-left-color: #e5484d; }"
		".note label { color: %s; }"
		".note .summary { font-weight: bold; }"
		".note .body { color: %s; }"
		".note button { background: %s; color: %s; border: none; box-shadow: none;"
		"  background-image: none; padding: 4px 10px; }"
		".note button:hover { background: %s; }",
		cfg.pal.bg, cfg.pal.sep, cfg.pal.accent, cfg.pal.text, cfg.pal.dim,
		cfg.pal.button, cfg.pal.text, cfg.pal.hover);
	if (!css) {
		css = gtk_css_provider_new();
		gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
			GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}
	gtk_css_provider_load_from_data(css, text, -1, NULL);
	g_free(text);
}

/* Stacks the boxes in the corner next to the taskbar: bottom right above a
 * bottom taskbar, top right below a top one. The newest is nearest to it. */
static void place_all(void)
{
	GdkRectangle area;
	GdkMonitor *mon = gdk_display_get_primary_monitor(gdk_display_get_default());
	int top = cfg.panel_position == PANEL_TOP, y;

	if (!mon)
		mon = gdk_display_get_monitor(gdk_display_get_default(), 0);
	gdk_monitor_get_geometry(mon, &area);
	y = top ? area.y + cfg.panel_height + GAP
	        : area.y + area.height - cfg.panel_height - GAP;
	for (GList *l = notes; l; l = l->next) {
		struct note *n = l->data;
		int w, h;

		/* a popup's size is what its contents need (at least BOX_W wide) */
		gtk_widget_get_preferred_width(n->win, NULL, &w);
		gtk_widget_get_preferred_height_for_width(n->win, w, NULL, &h);
		if (!top)
			y -= h;
		gtk_window_move(GTK_WINDOW(n->win), area.x + area.width - w - GAP, y);
		y += top ? h + GAP : -GAP;
	}
}

/* --- Showing and closing --------------------------------------------------------- */

static struct note *find(guint id)
{
	for (GList *l = notes; l; l = l->next)
		if (((struct note *)l->data)->id == id)
			return l->data;
	return NULL;
}

static void emit(const char *signal, GVariant *args)
{
	if (bus)
		g_dbus_connection_emit_signal(bus, NULL, "/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", signal, args, NULL);
}

static void close_note(struct note *n, guint reason)
{
	notes = g_list_remove(notes, n);
	if (n->timer)
		g_source_remove(n->timer);
	gtk_widget_destroy(n->win);
	emit("NotificationClosed", g_variant_new("(uu)", n->id, reason));
	g_free(n->default_action);
	g_free(n);
	place_all();
}

static gboolean on_timeout(gpointer data)
{
	struct note *n = data;

	n->timer = 0;
	close_note(n, CLOSED_EXPIRED);
	return G_SOURCE_REMOVE;
}

/* A click on the box: its default action (if any), then away with it */
static gboolean on_click(GtkWidget *w, GdkEventButton *e, gpointer data)
{
	struct note *n = data;

	(void)w; (void)e;
	if (n->default_action)
		emit("ActionInvoked", g_variant_new("(us)", n->id, n->default_action));
	close_note(n, CLOSED_DISMISSED);
	return TRUE;
}

static void on_action(GtkButton *b, gpointer data)
{
	struct note *n = data;

	emit("ActionInvoked", g_variant_new("(us)", n->id,
		(const char *)g_object_get_data(G_OBJECT(b), "key")));
	close_note(n, CLOSED_DISMISSED);
}

/* An icon name ("camera-photo") or a file (/path or file:///path) */
static GtkWidget *make_icon(const char *icon)
{
	if (!icon || !*icon)
		return NULL;
	if (g_str_has_prefix(icon, "file://"))
		icon += 7;
	if (icon[0] == '/') {
		GdkPixbuf *p = gdk_pixbuf_new_from_file_at_size(icon, 40, 40, NULL);
		GtkWidget *img = p ? gtk_image_new_from_pixbuf(p) : NULL;

		if (p)
			g_object_unref(p);
		return img;
	}
	if (!gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), icon))
		return NULL;
	return gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DIALOG);
}

static void label_class(GtkWidget *l, const char *cls)
{
	gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
}

static guint show_note(guint replaces, const char *icon, const char *summary,
                       const char *body, const char **actions, int critical,
                       int timeout)
{
	struct note *n = replaces ? find(replaces) : NULL;
	GtkWidget *win, *frame, *row, *text, *l, *img, *buttons = NULL;
	guint id;

	if (n) {                                  /* replace: same id, new box */
		id = n->id;
		notes = g_list_remove(notes, n);
		if (n->timer)
			g_source_remove(n->timer);
		gtk_widget_destroy(n->win);
		g_free(n->default_action);
		g_free(n);
	} else {
		id = next_id++;
	}
	while (g_list_length(notes) >= MAX_SHOWN)   /* too many: the oldest goes */
		close_note(g_list_last(notes)->data, CLOSED_EXPIRED);

	load_style();                             /* the theme may have changed */
	n = g_new0(struct note, 1);
	n->id = id;

	win = gtk_window_new(GTK_WINDOW_POPUP);
	gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
	gtk_widget_set_size_request(win, BOX_W, -1);
	gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
	n->win = win;

	frame = gtk_event_box_new();
	label_class(frame, "note");
	if (critical)
		label_class(frame, "critical");
	gtk_container_add(GTK_CONTAINER(win), frame);
	g_signal_connect(frame, "button-press-event", G_CALLBACK(on_click), n);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_container_set_border_width(GTK_CONTAINER(row), 12);
	gtk_container_add(GTK_CONTAINER(frame), row);
	if ((img = make_icon(icon))) {
		gtk_widget_set_valign(img, GTK_ALIGN_START);
		gtk_box_pack_start(GTK_BOX(row), img, FALSE, FALSE, 0);
	}
	text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);

	l = gtk_label_new(summary);
	label_class(l, "summary");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 30);
	gtk_box_pack_start(GTK_BOX(text), l, FALSE, FALSE, 0);
	if (body && *body) {
		l = gtk_label_new(body);              /* plain text, no markup */
		label_class(l, "body");
		gtk_label_set_xalign(GTK_LABEL(l), 0);
		gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 30);
		gtk_label_set_lines(GTK_LABEL(l), 6);
		gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
		gtk_box_pack_start(GTK_BOX(text), l, FALSE, FALSE, 0);
	}

	/* actions come in pairs: key, label. "default" is a click on the box. */
	for (int i = 0; actions && actions[i] && actions[i + 1]; i += 2) {
		GtkWidget *b;

		if (strcmp(actions[i], "default") == 0) {
			n->default_action = g_strdup(actions[i]);
			continue;
		}
		if (!buttons) {
			buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
			gtk_box_pack_start(GTK_BOX(text), buttons, FALSE, FALSE, 4);
		}
		b = gtk_button_new_with_label(actions[i + 1]);
		g_object_set_data_full(G_OBJECT(b), "key", g_strdup(actions[i]), g_free);
		g_signal_connect(b, "clicked", G_CALLBACK(on_action), n);
		gtk_box_pack_start(GTK_BOX(buttons), b, FALSE, FALSE, 0);
	}

	/* -1: our default; 0: stay until clicked; critical ones stay too */
	if (timeout < 0)
		timeout = critical ? 0 : TIMEOUT_MS;
	if (timeout > 0)
		n->timer = g_timeout_add(timeout, on_timeout, n);

	notes = g_list_prepend(notes, n);
	gtk_widget_show_all(win);
	place_all();
	return id;
}

/* --- D-Bus ----------------------------------------------------------------------- */

static void on_call(GDBusConnection *c, const char *sender, const char *path,
                    const char *iface, const char *method, GVariant *params,
                    GDBusMethodInvocation *inv, gpointer data)
{
	(void)c; (void)sender; (void)path; (void)iface; (void)data;

	if (strcmp(method, "Notify") == 0) {
		const char *app, *icon, *summary, *body, **actions = NULL;
		guint replaces, id;
		gint timeout;
		guchar urgency = 1;
		GVariant *hints, *v;
		const char *image = NULL;

		g_variant_get(params, "(&su&s&s&s^a&s@a{sv}i)", &app, &replaces, &icon,
		              &summary, &body, &actions, &hints, &timeout);
		if ((v = g_variant_lookup_value(hints, "urgency", G_VARIANT_TYPE_BYTE))) {
			urgency = g_variant_get_byte(v);
			g_variant_unref(v);
		}
		/* a picture instead of an icon (e.g. a screenshot's file) */
		if (!g_variant_lookup(hints, "image-path", "&s", &image))
			g_variant_lookup(hints, "image_path", "&s", &image);
		id = show_note(replaces, image && *image ? image : icon, summary, body,
		               actions, urgency >= 2, timeout);
		g_free(actions);
		g_variant_unref(hints);
		g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
	} else if (strcmp(method, "CloseNotification") == 0) {
		guint id;
		struct note *n;

		g_variant_get(params, "(u)", &id);
		if ((n = find(id)))
			close_note(n, CLOSED_BY_CALL);
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (strcmp(method, "GetCapabilities") == 0) {
		const char *caps[] = { "body", "actions", "icon-static", NULL };

		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(^as)", caps));
	} else if (strcmp(method, "GetServerInformation") == 0) {
		g_dbus_method_invocation_return_value(inv,
			g_variant_new("(ssss)", "my-notifyd", "my-distro", "1.0", "1.2"));
	} else {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_UNKNOWN_METHOD, "No method %s", method);
	}
}

static const GDBusInterfaceVTable vtable = { on_call, NULL, NULL, { 0 } };

static void on_bus(GDBusConnection *c, const char *name, gpointer data)
{
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection, NULL);

	(void)name; (void)data;
	bus = c;
	g_dbus_connection_register_object(c, "/org/freedesktop/Notifications",
		info->interfaces[0], &vtable, NULL, NULL, NULL);
	g_dbus_node_info_unref(info);
}

static void on_name_lost(GDBusConnection *c, const char *name, gpointer data)
{
	(void)data;
	if (!c)
		g_printerr("my-notifyd: no session D-Bus (DBUS_SESSION_BUS_ADDRESS)\n");
	else
		g_printerr("my-notifyd: another program shows notifications (%s)\n", name);
	gtk_main_quit();
}

int main(int argc, char **argv)
{
	gtk_init(&argc, &argv);
	load_style();
	g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
	               G_BUS_NAME_OWNER_FLAGS_NONE, on_bus, NULL, on_name_lost,
	               NULL, NULL);
	gtk_main();
	return 0;
}
