/*
 * my-bluetooth: the Bluetooth window of my-distro (GTK 3). The taskbar's
 * Bluetooth sign and the Start menu open it.
 *
 * bluetoothd (BlueZ, /etc/init.d/S40bluetoothd) does the real work. This
 * window talks to it over the system D-Bus, like bluetoothctl does:
 *   org.freedesktop.DBus.ObjectManager   which adapters and devices exist
 *   org.bluez.Adapter1    Powered (on/off), StartDiscovery (search), RemoveDevice
 *   org.bluez.Device1     Pair, Connect, Disconnect, Trusted
 *   org.bluez.Agent1      we answer bluetoothd's questions while pairing
 *                         ("does the device show 123456?", "type this PIN")
 * Members of the group netdev may do this (/etc/dbus-1/system.d/my-bluetooth.conf).
 *
 * Headphones and speakers: bluez-alsa (S41bluealsa) makes a connected one an
 * ALSA sound device. `my-bluetooth --watch` (started at login) notices when
 * one connects and makes it the default sound output of your programs, with
 * a few lines in ~/.asoundrc; when it disconnects they are removed again.
 * Programs that are already playing keep the old output until restarted.
 */
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLUEZ       "org.bluez"
#define AGENT_PATH  "/org/mydistro/bluetooth/agent"
#define SEARCH_SECS 25
#define ASOUNDRC    ".asoundrc"           /* in the home folder */
#define MARK_START  "# my-bluetooth start"
#define MARK_END    "# my-bluetooth end"

/* UUIDs of the audio profiles we can send sound to */
#define UUID_A2DP_SINK  "0000110b-0000-1000-8000-00805f9b34fb"
#define UUID_HFP        "0000111e-0000-1000-8000-00805f9b34fb"

struct dev {
	char path[128];
	char name[128];
	char address[18];
	char icon[48];          /* e.g. audio-headphones, input-keyboard */
	int paired, connected, trusted, audio;
	int rssi;               /* signal while searching; 0 = unknown */
};

static GDBusConnection *bus;
static char adapter[128];         /* object path, e.g. /org/bluez/hci0 */
static int powered, discovering;
static struct dev devs[64];
static int ndevs;

static GtkWidget *win, *status_label, *power_switch, *list, *btn_search, *btn_connect,
                 *btn_disconnect, *btn_forget;
static guint refresh_id, search_timer;
static int busy;                  /* pairing or connecting: buttons wait */
static int updating_switch;

/* --- Reading what bluetoothd knows -------------------------------------------------- */

static GVariant *managed_objects(void)
{
	GError *err = NULL;
	GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ, "/",
		"org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL,
		G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);

	if (!r) {
		g_clear_error(&err);
		return NULL;
	}
	return r;
}

static void str_prop(GVariant *props, const char *key, char *out, size_t len)
{
	const char *s;

	if (g_variant_lookup(props, key, "&s", &s))
		g_strlcpy(out, s, len);
}

static int bool_prop(GVariant *props, const char *key)
{
	gboolean b = FALSE;

	g_variant_lookup(props, key, "b", &b);
	return b;
}

static int has_audio(GVariant *props)
{
	GVariant *uuids = g_variant_lookup_value(props, "UUIDs", G_VARIANT_TYPE("as"));
	int audio = 0;

	if (uuids) {
		const char **u = g_variant_get_strv(uuids, NULL);

		for (int i = 0; u[i]; i++)
			audio |= !g_ascii_strcasecmp(u[i], UUID_A2DP_SINK) ||
			         !g_ascii_strcasecmp(u[i], UUID_HFP);
		g_free(u);
		g_variant_unref(uuids);
	}
	return audio;
}

/* Connected devices first, then paired ones, then by signal */
static int by_state(const void *a, const void *b)
{
	const struct dev *x = a, *y = b;

	if (x->connected != y->connected)
		return y->connected - x->connected;
	if (x->paired != y->paired)
		return y->paired - x->paired;
	return y->rssi - x->rssi;
}

/* Fills adapter, powered, discovering and devs[]. 0 = no adapter. */
static int read_state(void)
{
	GVariant *r = managed_objects(), *objs, *ifaces;
	GVariantIter it;
	const char *path;

	adapter[0] = '\0';
	ndevs = 0;
	if (!r)
		return 0;
	objs = g_variant_get_child_value(r, 0);
	/* the first adapter */
	g_variant_iter_init(&it, objs);
	while (!adapter[0] && g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *a = g_variant_lookup_value(ifaces, "org.bluez.Adapter1", NULL);

		if (a) {
			g_strlcpy(adapter, path, sizeof adapter);
			powered = bool_prop(a, "Powered");
			discovering = bool_prop(a, "Discovering");
			g_variant_unref(a);
		}
		g_variant_unref(ifaces);
	}
	/* its devices */
	g_variant_iter_init(&it, objs);
	while (adapter[0] && g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		GVariant *d = g_variant_lookup_value(ifaces, "org.bluez.Device1", NULL);
		const char *ad;

		if (d && g_variant_lookup(d, "Adapter", "&o", &ad) && !strcmp(ad, adapter) &&
		    ndevs < (int)G_N_ELEMENTS(devs)) {
			struct dev *v = &devs[ndevs++];
			gint16 rssi = 0;

			memset(v, 0, sizeof *v);
			g_strlcpy(v->path, path, sizeof v->path);
			str_prop(d, "Address", v->address, sizeof v->address);
			str_prop(d, "Alias", v->name, sizeof v->name);    /* Name, or the address */
			str_prop(d, "Icon", v->icon, sizeof v->icon);
			v->paired = bool_prop(d, "Paired");
			v->connected = bool_prop(d, "Connected");
			v->trusted = bool_prop(d, "Trusted");
			v->audio = has_audio(d);
			if (g_variant_lookup(d, "RSSI", "n", &rssi))
				v->rssi = rssi;
			/* while searching, nameless gadgets nearby show up by the
			 * dozen (only an address): leave those out */
			if (!v->paired && !v->connected && !g_variant_lookup(d, "Name", "&s", &ad))
				ndevs--;
		}
		if (d)
			g_variant_unref(d);
		g_variant_unref(ifaces);
	}
	g_variant_unref(objs);
	g_variant_unref(r);
	qsort(devs, ndevs, sizeof devs[0], by_state);
	return adapter[0] != '\0';
}

/* Calls a method of bluetoothd and waits (up to timeout ms). NULL = ok,
 * else an error message to show (g_free it). */
static char *call(const char *path, const char *iface, const char *method,
                  GVariant *args, int timeout)
{
	GError *err = NULL;
	GVariant *r = g_dbus_connection_call_sync(bus, BLUEZ, path, iface, method, args,
		NULL, G_DBUS_CALL_FLAGS_NONE, timeout, NULL, &err);
	char *msg;

	if (r) {
		g_variant_unref(r);
		return NULL;
	}
	g_dbus_error_strip_remote_error(err);
	msg = g_strdup(err->message);
	g_error_free(err);
	return msg;
}

static char *set_prop(const char *path, const char *iface, const char *prop, GVariant *v)
{
	return call(path, "org.freedesktop.DBus.Properties", "Set",
	            g_variant_new("(ssv)", iface, prop, v), 5000);
}

/* --- Sound to headphones (~/.asoundrc) ----------------------------------------------- */

/* Rewrites ~/.asoundrc without our block, and with a new one for address
 * (NULL: none). Lines of your own stay as they are. */
static void set_sound_output(const char *address, const char *name)
{
	char *path = g_build_filename(g_get_home_dir(), ASOUNDRC, NULL), *old = NULL;
	GString *s = g_string_new(NULL);
	int skip = 0;

	if (g_file_get_contents(path, &old, NULL, NULL)) {
		char **lines = g_strsplit(old, "\n", -1);

		for (int i = 0; lines[i]; i++) {
			if (g_str_has_prefix(lines[i], MARK_START))
				skip = 1;
			if (!skip && (lines[i][0] || lines[i + 1]))
				g_string_append_printf(s, "%s\n", lines[i]);
			if (g_str_has_prefix(lines[i], MARK_END))
				skip = 0;
		}
		g_strfreev(lines);
	}
	if (address) {
		/* the name comes from the device: no line breaks or quotes in it */
		char *n = g_strdelimit(g_strdup(name), "\r\n\"\\", ' ');

		g_string_append_printf(s,
			MARK_START ": while \"%s\" is connected, programs play their\n"
			"# sound there. my-bluetooth --watch adds and removes these lines.\n"
			"pcm.!default {\n\ttype plug\n\tslave.pcm {\n\t\ttype bluealsa\n"
			"\t\tdevice \"%s\"\n\t\tprofile \"a2dp\"\n\t}\n}\n" MARK_END "\n",
			n, address);
		g_free(n);
	}
	if (s->len == 0)
		g_unlink(path);           /* nothing of yours in it either */
	else if (!old || strcmp(old, s->str) != 0)
		g_file_set_contents(path, s->str, -1, NULL);
	g_free(old);
	g_free(path);
	g_string_free(s, TRUE);
}

/* The connected audio device, if any; NULL otherwise */
static struct dev *connected_audio(void)
{
	for (int i = 0; i < ndevs; i++)
		if (devs[i].connected && devs[i].audio)
			return &devs[i];
	return NULL;
}

static char last_audio[18];       /* --watch: the headphones we switched to */

static void follow_audio(void)
{
	struct dev *d = connected_audio();

	if (d && strcmp(d->address, last_audio) != 0) {
		const char *argv[] = { "my-notify", "-i", "audio-headphones", d->name,
			"Sound now plays here. Programs that were already playing: start them again.",
			NULL };

		set_sound_output(d->address, d->name);
		g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
		g_strlcpy(last_audio, d->address, sizeof last_audio);
	} else if (!d && last_audio[0]) {
		set_sound_output(NULL, NULL);
		last_audio[0] = '\0';
	}
}

/* --- The window ---------------------------------------------------------------------- */

static struct dev *selected(void)
{
	GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
	int i = r ? gtk_list_box_row_get_index(r) : -1;

	return i >= 0 && i < ndevs ? &devs[i] : NULL;
}

static void update_buttons(void)
{
	struct dev *d = selected();

	gtk_widget_set_sensitive(btn_search, powered && !busy);
	gtk_widget_set_sensitive(btn_connect, d && !d->connected && powered && !busy);
	gtk_widget_set_sensitive(btn_disconnect, d && d->connected && !busy);
	gtk_widget_set_sensitive(btn_forget, d && (d->paired || d->trusted) && !busy);
}

static void status(const char *text)
{
	gtk_label_set_text(GTK_LABEL(status_label), text);
}

static void fill_list(void)
{
	GList *old = gtk_container_get_children(GTK_CONTAINER(list));
	char sel[128] = "";
	struct dev *s = selected();

	if (s)
		g_strlcpy(sel, s->path, sizeof sel);
	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);

	for (int i = 0; i < ndevs; i++) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
		GtkWidget *icon = gtk_image_new_from_icon_name(devs[i].icon[0] ? devs[i].icon : "my-bluetooth",
		                                               GTK_ICON_SIZE_LARGE_TOOLBAR);
		GtkWidget *name = gtk_label_new(NULL), *info = gtk_label_new(NULL);
		char *m = g_markup_printf_escaped(devs[i].connected ? "<b>%s</b>" : "%s", devs[i].name);

		gtk_label_set_markup(GTK_LABEL(name), m);
		gtk_label_set_xalign(GTK_LABEL(name), 0);
		gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
		gtk_label_set_text(GTK_LABEL(info), devs[i].connected ? "connected" :
		                   devs[i].paired ? "paired" : "new");
		gtk_style_context_add_class(gtk_widget_get_style_context(info), "dim-label");
		g_object_set(row, "margin", 8, NULL);
		gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
		gtk_box_pack_end(GTK_BOX(row), info, FALSE, FALSE, 0);
		gtk_list_box_insert(GTK_LIST_BOX(list), row, -1);
		if (sel[0] && !strcmp(sel, devs[i].path))
			gtk_list_box_select_row(GTK_LIST_BOX(list),
			                        gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i));
		g_free(m);
	}
	gtk_widget_show_all(list);
}

static gboolean refresh(gpointer d)
{
	(void)d;
	refresh_id = 0;
	if (!read_state()) {
		status("No Bluetooth adapter found. This computer has none, it is switched off "
		       "(some laptops have a key or switch for it), or my-distro has no driver "
		       "for it. A USB Bluetooth stick works too.");
		gtk_widget_set_sensitive(power_switch, FALSE);
		ndevs = 0;
	} else {
		gtk_widget_set_sensitive(power_switch, TRUE);
		updating_switch = 1;
		gtk_switch_set_active(GTK_SWITCH(power_switch), powered);
		updating_switch = 0;
		if (!busy)
			status(!powered ? "Bluetooth is off." :
			       discovering ? "Looking for devices… Put your device into pairing "
			                     "mode (see its manual; often: hold its power button)." :
			       ndevs ? "Choose a device and click Connect." :
			               "Click Search to find devices nearby.");
	}
	fill_list();
	update_buttons();
	return G_SOURCE_REMOVE;
}

/* bluetoothd announces every change: redraw once things calm down */
static void on_bluez_signal(GDBusConnection *c, const char *sender, const char *path,
                            const char *iface, const char *sig, GVariant *params, gpointer d)
{
	(void)c; (void)sender; (void)path; (void)iface; (void)sig; (void)params; (void)d;
	if (win && !refresh_id)
		refresh_id = g_timeout_add(300, refresh, NULL);
	if (!win) {                       /* --watch */
		read_state();
		follow_audio();
	}
}

static void error_box(const char *what, const char *msg)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", what);

	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", msg);
	gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
}

static void on_power(GObject *o, GParamSpec *ps, gpointer d)
{
	char *err;

	(void)ps; (void)d;
	if (updating_switch || !adapter[0])
		return;
	err = set_prop(adapter, "org.bluez.Adapter1", "Powered",
	               g_variant_new_boolean(gtk_switch_get_active(GTK_SWITCH(o))));
	if (err) {
		error_box("Bluetooth could not be switched on.",
		          strstr(err, "rfkill") || strstr(err, "Blocked") ?
		          "It is switched off with a key or a switch on the laptop "
		          "(often Fn with an aeroplane or Bluetooth sign). Switch it on there." : err);
		g_free(err);
	}
	refresh(NULL);
}

static gboolean stop_search(gpointer d)
{
	(void)d;
	search_timer = 0;
	g_free(call(adapter, "org.bluez.Adapter1", "StopDiscovery", NULL, 5000));
	refresh(NULL);
	return G_SOURCE_REMOVE;
}

static void on_search(GtkButton *b, gpointer d)
{
	char *err;

	(void)b; (void)d;
	err = call(adapter, "org.bluez.Adapter1", "StartDiscovery", NULL, 5000);
	if (err && !strstr(err, "InProgress") && !strstr(err, "progress")) {
		error_box("Could not search for devices.", err);
		g_free(err);
		return;
	}
	g_free(err);
	if (search_timer)
		g_source_remove(search_timer);
	search_timer = g_timeout_add_seconds(SEARCH_SECS, stop_search, NULL);
	refresh(NULL);
}

/* Pair (first time), trust (connects by itself later) and connect. Pairing
 * can take a while: bluetoothd may ask us questions meanwhile (the agent
 * below), so the window stays responsive while we wait. */
struct job { char path[128], name[128]; int step; };

static void job_next(struct job *j);

static void job_done(GObject *src, GAsyncResult *res, gpointer data)
{
	struct job *j = data;
	GError *err = NULL;
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

	if (r) {
		g_variant_unref(r);
		j->step++;
		job_next(j);
		return;
	}
	g_dbus_error_strip_remote_error(err);
	if (j->step == 0 && strstr(err->message, "AlreadyExists")) {   /* paired already */
		g_error_free(err);
		j->step++;
		job_next(j);
		return;
	}
	busy = 0;
	{
		char *what = g_strdup_printf(j->step == 0 ? "Could not pair with %s." :
		                             "Could not connect to %s.", j->name);

		error_box(what, strstr(err->message, "AuthenticationFailed") ||
		                strstr(err->message, "AuthenticationRejected") ?
		          "Pairing was refused. Put the device into pairing mode and try again." :
		          strstr(err->message, "Timeout") || strstr(err->message, "Page") ?
		          "It didn't answer. Is it switched on, nearby and in pairing mode?" :
		          err->message);
		g_free(what);
	}
	g_error_free(err);
	g_free(j);
	refresh(NULL);
}

static void job_next(struct job *j)
{
	char *err;

	switch (j->step) {
	case 0:
		status("Pairing… If the device shows a number or asks for one, follow it.");
		g_dbus_connection_call(bus, BLUEZ, j->path, "org.bluez.Device1", "Pair", NULL,
		                       NULL, G_DBUS_CALL_FLAGS_NONE, 90000, NULL, job_done, j);
		return;
	case 1:
		/* trusted: may connect by itself next time (headphones, mice) */
		err = set_prop(j->path, "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE));
		g_free(err);
		status("Connecting…");
		g_dbus_connection_call(bus, BLUEZ, j->path, "org.bluez.Device1", "Connect", NULL,
		                       NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, job_done, j);
		return;
	default:
		busy = 0;
		status("Connected.");
		g_free(j);
		refresh(NULL);
	}
}

static void on_connect(GtkButton *b, gpointer d)
{
	struct dev *v = selected();
	struct job *j;

	(void)b; (void)d;
	if (!v || v->connected || busy)
		return;
	j = g_new0(struct job, 1);
	g_strlcpy(j->path, v->path, sizeof j->path);
	g_strlcpy(j->name, v->name, sizeof j->name);
	j->step = v->paired ? 1 : 0;
	if (search_timer) {               /* searching slows connecting down */
		g_source_remove(search_timer);
		stop_search(NULL);
	}
	busy = 1;
	update_buttons();
	job_next(j);
}

static void on_disconnect(GtkButton *b, gpointer d)
{
	struct dev *v = selected();

	(void)b; (void)d;
	if (v)
		g_free(call(v->path, "org.bluez.Device1", "Disconnect", NULL, 10000));
	refresh(NULL);
}

static void on_forget(GtkButton *b, gpointer d)
{
	struct dev *v = selected();
	char *err;

	(void)b; (void)d;
	if (!v)
		return;
	err = call(adapter, "org.bluez.Adapter1", "RemoveDevice",
	           g_variant_new("(o)", v->path), 10000);
	if (err) {
		error_box("Could not forget the device.", err);
		g_free(err);
	}
	refresh(NULL);
}

static void on_row(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)r; (void)d;
	update_buttons();
}

static void on_row_activated(GtkListBox *l, GtkListBoxRow *r, gpointer d)
{
	(void)l; (void)r; (void)d;
	on_connect(NULL, NULL);
}

/* --- The agent: questions from bluetoothd while pairing ------------------------------ */

static const char agent_xml[] =
	"<node><interface name='org.bluez.Agent1'>"
	"<method name='Release'/>"
	"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
	"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/>"
	"<arg type='q' direction='in'/></method>"
	"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
	"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
	"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='Cancel'/>"
	"</interface></node>";

static GtkWidget *shown;          /* "type 123456 on the keyboard" while pairing */

static const char *name_of(const char *path)
{
	for (int i = 0; i < ndevs; i++)
		if (!strcmp(devs[i].path, path))
			return devs[i].name;
	return "the device";
}

static int ask_yes_no(const char *question, const char *detail)
{
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_MODAL,
		GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", question);
	int r;

	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail);
	r = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(d);
	return r == GTK_RESPONSE_YES;
}

/* Asks for a PIN or passkey. NULL = cancelled. */
static char *ask_code(const char *question)
{
	GtkWidget *d = gtk_dialog_new_with_buttons("Bluetooth", GTK_WINDOW(win), GTK_DIALOG_MODAL,
		"Cancel", GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);
	GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d)), *e = gtk_entry_new();
	char *code = NULL;

	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_box_set_spacing(GTK_BOX(box), 8);
	gtk_box_pack_start(GTK_BOX(box), gtk_label_new(question), FALSE, FALSE, 0);
	gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
	gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
	gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
	gtk_widget_show_all(d);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK)
		code = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
	gtk_widget_destroy(d);
	return code;
}

static void show_code(const char *text)
{
	if (shown)
		gtk_widget_destroy(shown);
	shown = gtk_message_dialog_new(GTK_WINDOW(win), 0, GTK_MESSAGE_INFO,
	                               GTK_BUTTONS_CLOSE, "%s", text);
	g_signal_connect(shown, "response", G_CALLBACK(gtk_widget_destroy), NULL);
	g_signal_connect(shown, "destroy", G_CALLBACK(gtk_widget_destroyed), &shown);
	gtk_widget_show(shown);
}

static void reject(GDBusMethodInvocation *inv)
{
	g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Rejected");
}

static void agent_call(GDBusConnection *c, const char *sender, const char *path,
                       const char *iface, const char *method, GVariant *p,
                       GDBusMethodInvocation *inv, gpointer d)
{
	const char *dev = NULL;
	char text[300], *code;
	guint32 key = 0;

	(void)c; (void)sender; (void)path; (void)iface; (void)d;
	if (g_variant_n_children(p) > 0) {              /* the device it is about */
		GVariant *first = g_variant_get_child_value(p, 0);

		if (g_variant_is_of_type(first, G_VARIANT_TYPE_OBJECT_PATH))
			g_variant_get_child(p, 0, "&o", &dev);
		g_variant_unref(first);
	}

	if (!strcmp(method, "RequestConfirmation")) {
		g_variant_get(p, "(&ou)", &dev, &key);
		snprintf(text, sizeof text, "Does %s show the number %06u?", name_of(dev), key);
		if (ask_yes_no(text, "Then click Yes (and confirm on the device, if it asks too)."))
			g_dbus_method_invocation_return_value(inv, NULL);
		else
			reject(inv);
	} else if (!strcmp(method, "RequestPinCode") || !strcmp(method, "RequestPasskey")) {
		snprintf(text, sizeof text, "PIN for %s (often 0000 or 1234, see its manual):",
		         name_of(dev));
		if (!(code = ask_code(text)) || !code[0])
			reject(inv);
		else if (!strcmp(method, "RequestPinCode"))              /* text */
			g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", code));
		else
			g_dbus_method_invocation_return_value(inv,
				g_variant_new("(u)", (guint32)strtoul(code, NULL, 10)));
		g_free(code);
	} else if (!strcmp(method, "DisplayPasskey") || !strcmp(method, "DisplayPinCode")) {
		if (!strcmp(method, "DisplayPasskey")) {
			guint16 typed;

			g_variant_get(p, "(&ouq)", &dev, &key, &typed);
			snprintf(text, sizeof text, "Type %06u on %s, then press Enter on it.",
			         key, name_of(dev));
		} else {
			const char *pin;

			g_variant_get(p, "(&o&s)", &dev, &pin);
			snprintf(text, sizeof text, "Type %s on %s, then press Enter on it.",
			         pin, name_of(dev));
		}
		show_code(text);
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (!strcmp(method, "RequestAuthorization")) {
		snprintf(text, sizeof text, "Pair with %s?", name_of(dev));
		if (ask_yes_no(text, "It asks to be paired with this computer."))
			g_dbus_method_invocation_return_value(inv, NULL);
		else
			reject(inv);
	} else {
		/* AuthorizeService (a paired device uses a service): yes.
		 * Cancel, Release: pairing ended. */
		if (shown && strcmp(method, "AuthorizeService") != 0)
			gtk_widget_destroy(shown);
		g_dbus_method_invocation_return_value(inv, NULL);
	}
}

static void register_agent(void)
{
	static const GDBusInterfaceVTable vt = { agent_call, NULL, NULL, { 0 } };
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(agent_xml, NULL);

	if (!info)
		return;
	g_dbus_connection_register_object(bus, AGENT_PATH, info->interfaces[0], &vt,
	                                  NULL, NULL, NULL);
	/* KeyboardDisplay: we can show a number and type one */
	g_free(call("/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
	            g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"), 5000));
	g_free(call("/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent",
	            g_variant_new("(o)", AGENT_PATH), 5000));
	g_dbus_node_info_unref(info);
}

static void watch_bluez(void)
{
	g_dbus_connection_signal_subscribe(bus, BLUEZ, NULL, NULL, NULL, NULL,
	                                   G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_signal, NULL, NULL);
}

static GtkWidget *button(const char *text, GCallback cb)
{
	GtkWidget *b = gtk_button_new_with_label(text);

	g_signal_connect(b, "clicked", cb, NULL);
	return b;
}

int main(int argc, char **argv)
{
	GtkWidget *box, *top, *scroll, *row;
	GError *err = NULL;

	/* --watch: no window. Keeps the sound output on connected headphones
	 * (see the top of this file); started at login. */
	if (argc > 1 && !strcmp(argv[1], "--watch")) {
		GMainLoop *loop = g_main_loop_new(NULL, FALSE);

		if (!(bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL)))
			return 1;
		set_sound_output(NULL, NULL);       /* left over from a crash: gone */
		read_state();
		follow_audio();
		watch_bluez();
		g_main_loop_run(loop);
		return 0;
	}

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(win), "Bluetooth");
	gtk_window_set_default_size(GTK_WINDOW(win), 520, 460);
	gtk_window_set_icon_name(GTK_WINDOW(win), "my-bluetooth");
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(box), 16);
	gtk_container_add(GTK_CONTAINER(win), box);

	top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_box_pack_start(GTK_BOX(top), gtk_label_new("Bluetooth"), FALSE, FALSE, 0);
	power_switch = gtk_switch_new();
	g_signal_connect(power_switch, "notify::active", G_CALLBACK(on_power), NULL);
	gtk_box_pack_end(GTK_BOX(top), power_switch, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

	status_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(status_label), 0);
	gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
	gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

	list = gtk_list_box_new();
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), FALSE);
	g_signal_connect(list, "row-selected", G_CALLBACK(on_row), NULL);
	g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), list);
	gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	btn_search = button("Search", G_CALLBACK(on_search));
	btn_connect = button("Connect", G_CALLBACK(on_connect));
	gtk_style_context_add_class(gtk_widget_get_style_context(btn_connect), "suggested-action");
	btn_disconnect = button("Disconnect", G_CALLBACK(on_disconnect));
	btn_forget = button("Forget", G_CALLBACK(on_forget));
	gtk_box_pack_start(GTK_BOX(row), btn_search, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_connect, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_disconnect, FALSE, FALSE, 0);
	gtk_box_pack_end(GTK_BOX(row), btn_forget, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
	gtk_widget_show_all(win);

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus) {
		status("The system's message bus (D-Bus) is not running.");
		g_clear_error(&err);
	} else {
		watch_bluez();
		register_agent();
		refresh(NULL);
		if (adapter[0] && powered && ndevs == 0)
			on_search(NULL, NULL);        /* nothing known yet: look around */
	}
	update_buttons();
	gtk_main();
	if (bus && adapter[0] && discovering)
		g_free(call(adapter, "org.bluez.Adapter1", "StopDiscovery", NULL, 2000));
	return 0;
}
