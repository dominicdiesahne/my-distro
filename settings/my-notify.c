/*
 * my-notify (also called notify-send): shows a notification on the desktop.
 *
 *     my-notify [-i ICON] [-u low|normal|critical] [-t MS] [-a APP] [-p]
 *               SUMMARY [BODY]
 *
 *   -i  icon name from the icon theme (camera-photo) or a picture's path
 *   -u  critical ones stay until clicked
 *   -t  how long it stays, in milliseconds (0 = until clicked)
 *   -a  the program's name
 *   -p  print the notification's number
 *
 * It sends the standard Notify call over the session D-Bus to whatever
 * shows notifications (my-notifyd on my-distro); the same options as the
 * notify-send of other Linux systems, so scripts written for them work.
 */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
	fprintf(stderr, "usage: my-notify [-i ICON] [-u low|normal|critical] [-t MS] "
	        "[-a APP] [-p] SUMMARY [BODY]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *icon = "", *app = "my-notify", *summary = NULL, *body = "";
	const char *no_actions[] = { NULL };
	guchar urgency = 1;
	gint timeout = -1;
	int print_id = 0;
	GDBusConnection *bus;
	GVariantBuilder hints;
	GVariant *reply;
	GError *err = NULL;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] == '-' && a[1] && strchr("iuta", a[1]) && !a[2]) {
			if (i + 1 >= argc)
				usage();
			const char *v = argv[++i];

			switch (a[1]) {
			case 'i': icon = v; break;
			case 'a': app = v; break;
			case 't': timeout = atoi(v); break;
			case 'u': urgency = strcmp(v, "low") == 0 ? 0 :
			                    strcmp(v, "critical") == 0 ? 2 : 1; break;
			}
		} else if (strncmp(a, "--icon=", 7) == 0) {
			icon = a + 7;
		} else if (strncmp(a, "--urgency=", 10) == 0) {
			urgency = strcmp(a + 10, "low") == 0 ? 0 :
			          strcmp(a + 10, "critical") == 0 ? 2 : 1;
		} else if (strncmp(a, "--expire-time=", 14) == 0) {
			timeout = atoi(a + 14);
		} else if (strncmp(a, "--app-name=", 11) == 0) {
			app = a + 11;
		} else if (strcmp(a, "-p") == 0 || strcmp(a, "--print-id") == 0) {
			print_id = 1;
		} else if (strcmp(a, "--") == 0 && i + 1 < argc) {
			summary = argv[++i];
			if (i + 1 < argc)
				body = argv[++i];
			break;
		} else if (a[0] == '-' && a[1]) {
			usage();
		} else if (!summary) {
			summary = a;
		} else {
			body = a;
		}
	}
	if (!summary)
		usage();

	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!bus) {
		fprintf(stderr, "my-notify: no session D-Bus: %s\n", err->message);
		return 1;
	}
	g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(urgency));
	reply = g_dbus_connection_call_sync(bus, "org.freedesktop.Notifications",
		"/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
		g_variant_new("(susss^as@a{sv}i)", app, 0, icon, summary, body, no_actions,
		              g_variant_builder_end(&hints), timeout),
		G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
	if (!reply) {
		fprintf(stderr, "my-notify: %s\n", err->message);
		return 1;
	}
	if (print_id) {
		guint id;

		g_variant_get(reply, "(u)", &id);
		printf("%u\n", id);
	}
	g_variant_unref(reply);
	return 0;
}
