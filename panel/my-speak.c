/*
 * my-speak: reads the selected text aloud (Super+R, or Settings,
 * Accessibility). Select text with the mouse in any program first.
 * Run it again while it speaks: it stops.
 *
 *   my-speak            the selected text (or, if none, the copied text)
 *   my-speak TEXT...    that text
 *
 * The X selection is fetched with Xlib (like any program does when you
 * paste with the middle mouse button). eSpeak turns it into speech
 * (espeak --stdout) and aplay plays it on the sound card. English voice;
 * other languages: MY_SPEAK_VOICE=de my-speak (espeak --voices lists them).
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

/* The text of a selection (PRIMARY: selected, CLIPBOARD: copied), or NULL */
static char *selection(Display *dpy, Window w, Atom sel)
{
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	Atom prop = XInternAtom(dpy, "MY_SPEAK", False), type;
	int format;
	unsigned long n, left;
	unsigned char *data = NULL;
	char *text = NULL;
	XEvent e;

	if (XGetSelectionOwner(dpy, sel) == None)
		return NULL;
	XConvertSelection(dpy, sel, utf8, prop, w, CurrentTime);
	XFlush(dpy);
	/* wait up to a second for the owner's answer */
	for (int i = 0; i < 20; i++) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &e);
			if (e.type == SelectionNotify) {
				if (e.xselection.property == None)
					return NULL;
				if (XGetWindowProperty(dpy, w, prop, 0, 1 << 20, True, AnyPropertyType,
				                       &type, &format, &n, &left, &data) == Success &&
				    data && format == 8)
					text = strndup((char *)data, n);
				if (data)
					XFree(data);
				return text;
			}
		}
		usleep(50000);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	char pidfile[64], *text = NULL;
	const char *voice = getenv("MY_SPEAK_VOICE");
	FILE *f;
	int pid = 0;

	/* Speaking already: stop it */
	snprintf(pidfile, sizeof pidfile, "/tmp/my-speak.%d", (int)getuid());
	if ((f = fopen(pidfile, "r"))) {
		if (fscanf(f, "%d", &pid) != 1)
			pid = 0;
		fclose(f);
		unlink(pidfile);
		if (pid > 0 && kill(-pid, SIGTERM) == 0)
			return 0;
	}

	if (argc > 1) {
		size_t len = 1;

		for (int i = 1; i < argc; i++)
			len += strlen(argv[i]) + 1;
		text = calloc(1, len);
		for (int i = 1; i < argc && text; i++) {
			strcat(text, argv[i]);
			strcat(text, " ");
		}
	} else {
		Display *dpy = XOpenDisplay(NULL);
		Window w;

		if (!dpy) {
			fprintf(stderr, "my-speak: no X display\n");
			return 1;
		}
		w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
		text = selection(dpy, w, XA_PRIMARY);
		if (!text || !text[strspn(text, " \t\r\n")]) {
			free(text);
			text = selection(dpy, w, XInternAtom(dpy, "CLIPBOARD", False));
		}
		XCloseDisplay(dpy);
	}
	if (!text || !text[strspn(text, " \t\r\n")]) {
		execlp("my-notify", "my-notify", "-i", "audio-speakers", "Read aloud",
		       "Select some text with the mouse first, then press Super+R.", (char *)NULL);
		return 1;
	}

	/* Our own process group, so a second my-speak can stop all of it */
	setsid();
	if ((f = fopen(pidfile, "w"))) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}
	{
		char cmd[128];
		FILE *p;

		snprintf(cmd, sizeof cmd, "espeak --stdin --stdout -s 160 -v %s | aplay -q",
		         voice && strspn(voice, "abcdefghijklmnopqrstuvwxyz-+0123456789") == strlen(voice) &&
		         voice[0] ? voice : "en");
		p = popen(cmd, "w");
		if (p) {
			fputs(text, p);
			fputc('\n', p);
			pclose(p);
		}
	}
	unlink(pidfile);
	free(text);
	return 0;
}
