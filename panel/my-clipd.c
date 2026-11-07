/*
 * my-clipd: keeps copied text after the program it was copied from closes.
 *
 * On X11 the clipboard is not a store: "copy" only makes a program the
 * owner of the CLIPBOARD selection, and "paste" asks that owner for the
 * data. When the owner closes, the copied text is gone.
 *
 * my-clipd watches the owner (XFixes tells it about every change). After
 * each copy it fetches the text right away. When the owner then closes, it
 * takes over the clipboard and hands out that text itself. While the owner
 * is running, my-clipd stays out of the way, so pasting pictures or
 * formatted text inside a program works as before.
 *
 * Only text is kept. A program that empties the clipboard on purpose (a
 * password manager after a few seconds) is respected: the text is forgotten.
 *
 * Started at login by /usr/lib/my-distro/autostart.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#define MAX_TEXT (16 * 1024 * 1024)

static Display *dpy;
static Window win;
static Atom CLIPBOARD, UTF8, TARGETS, TEXT, INCR, PROP, TIMESTAMP, MULTIPLE;
static char *text;              /* the last copied text */
static size_t text_len;
static Time owned_since;        /* when we took over; 0 = we don't own it */

static void forget(void)
{
	if (text)
		memset(text, 0, text_len);
	free(text);
	text = NULL;
	text_len = 0;
}

/* Waits up to 2 seconds for an event of one type on our window */
static int wait_event(int type, XEvent *ev)
{
	time_t end = time(NULL) + 2;

	while (time(NULL) <= end) {
		if (XCheckTypedWindowEvent(dpy, win, type, ev))
			return 1;
		fd_set fds;
		struct timeval tv = { 0, 100000 };

		FD_ZERO(&fds);
		FD_SET(ConnectionNumber(dpy), &fds);
		select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, &tv);
	}
	return 0;
}

/* Fetches the clipboard's text from its owner. Large texts come in parts
 * (the INCR protocol): the owner writes part after part into our property,
 * we delete it each time to ask for the next one. */
static void fetch(Time when)
{
	XEvent ev;
	Atom type;
	int format;
	unsigned long n, after;
	unsigned char *data = NULL;
	char *got = NULL;
	size_t got_len = 0;

	XDeleteProperty(dpy, win, PROP);
	XConvertSelection(dpy, CLIPBOARD, UTF8, PROP, win, when);
	if (!wait_event(SelectionNotify, &ev) || ev.xselection.property == None)
		return;                       /* no text (e.g. a picture) */
	if (XGetWindowProperty(dpy, win, PROP, 0, MAX_TEXT / 4, True, AnyPropertyType,
	                       &type, &format, &n, &after, &data) != Success)
		return;
	if (type == INCR) {
		XFree(data);
		for (;;) {
			if (!wait_event(PropertyNotify, &ev))
				goto fail;
			if (ev.xproperty.atom != PROP || ev.xproperty.state != PropertyNewValue)
				continue;
			if (XGetWindowProperty(dpy, win, PROP, 0, MAX_TEXT / 4, True,
			                       AnyPropertyType, &type, &format, &n, &after,
			                       &data) != Success)
				goto fail;
			if (n == 0) {         /* an empty part: the end */
				XFree(data);
				break;
			}
			if (got_len + n > MAX_TEXT) {
				XFree(data);
				goto fail;
			}
			got = realloc(got, got_len + n + 1);
			memcpy(got + got_len, data, n);
			got_len += n;
			XFree(data);
		}
	} else if (format == 8 && n > 0) {
		got = malloc(n + 1);
		memcpy(got, data, n);
		got_len = n;
		XFree(data);
	} else {
		if (data)
			XFree(data);
		return;
	}
	if (got) {
		got[got_len] = '\0';
		forget();
		text = got;
		text_len = got_len;
	}
	return;
fail:
	if (got) {
		memset(got, 0, got_len);
		free(got);
	}
}

/* Another program asks us (the owner) for the clipboard */
static void answer(XSelectionRequestEvent *rq)
{
	XEvent reply = { 0 };
	Atom prop = rq->property != None ? rq->property : rq->target;

	reply.xselection.type = SelectionNotify;
	reply.xselection.requestor = rq->requestor;
	reply.xselection.selection = rq->selection;
	reply.xselection.target = rq->target;
	reply.xselection.time = rq->time;
	reply.xselection.property = None;

	if (rq->selection == CLIPBOARD && text) {
		if (rq->target == TARGETS) {
			Atom t[] = { TARGETS, TIMESTAMP, UTF8, XA_STRING, TEXT };

			XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
			                (unsigned char *)t, sizeof t / sizeof t[0]);
			reply.xselection.property = prop;
		} else if (rq->target == TIMESTAMP) {
			long t = (long)owned_since;

			XChangeProperty(dpy, rq->requestor, prop, XA_INTEGER, 32, PropModeReplace,
			                (unsigned char *)&t, 1);
			reply.xselection.property = prop;
		} else if (rq->target == UTF8 || rq->target == XA_STRING || rq->target == TEXT) {
			/* Very big texts would need INCR; X takes up to the maximum
			 * request size at once (with BIG-REQUESTS usually 16 MB). */
			long max = XExtendedMaxRequestSize(dpy) ? XExtendedMaxRequestSize(dpy)
			                                        : XMaxRequestSize(dpy);
			size_t len = text_len < (size_t)max * 4 - 100 ? text_len : (size_t)max * 4 - 100;

			XChangeProperty(dpy, rq->requestor, prop,
			                rq->target == XA_STRING ? XA_STRING : UTF8, 8,
			                PropModeReplace, (unsigned char *)text, (int)len);
			reply.xselection.property = prop;
		}
	}
	XSendEvent(dpy, rq->requestor, False, NoEventMask, &reply);
	XFlush(dpy);
}

static int ignore_errors(Display *d, XErrorEvent *e)
{
	(void)d; (void)e;
	return 0;
}

int main(void)
{
	int ev_base, err_base;
	XSetWindowAttributes a = { 0 };

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "my-clipd: can't open display\n");
		return 1;
	}
	if (!XFixesQueryExtension(dpy, &ev_base, &err_base)) {
		fprintf(stderr, "my-clipd: the X server has no XFixes extension\n");
		return 1;
	}
	XSetErrorHandler(ignore_errors);
	CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);
	UTF8      = XInternAtom(dpy, "UTF8_STRING", False);
	TARGETS   = XInternAtom(dpy, "TARGETS", False);
	TEXT      = XInternAtom(dpy, "TEXT", False);
	INCR      = XInternAtom(dpy, "INCR", False);
	TIMESTAMP = XInternAtom(dpy, "TIMESTAMP", False);
	MULTIPLE  = XInternAtom(dpy, "MULTIPLE", False);
	PROP      = XInternAtom(dpy, "MY_CLIPD", False);
	(void)MULTIPLE;

	/* An invisible window: it receives the text and is the owner later */
	a.event_mask = PropertyChangeMask;
	win = XCreateWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, CopyFromParent,
	                    InputOnly, CopyFromParent, CWEventMask, &a);
	XStoreName(dpy, win, "my-clipd");
	XFixesSelectSelectionInput(dpy, DefaultRootWindow(dpy), CLIPBOARD,
	                           XFixesSetSelectionOwnerNotifyMask |
	                           XFixesSelectionWindowDestroyNotifyMask |
	                           XFixesSelectionClientCloseNotifyMask);

	for (;;) {
		XEvent ev;

		XNextEvent(dpy, &ev);
		if (ev.type == ev_base + XFixesSelectionNotify) {
			XFixesSelectionNotifyEvent *s = (XFixesSelectionNotifyEvent *)&ev;

			if (s->selection != CLIPBOARD)
				continue;
			switch (s->subtype) {
			case XFixesSetSelectionOwnerNotify:
				if (s->owner == win)
					break;                /* that was us */
				owned_since = 0;
				if (s->owner == None)
					forget();             /* emptied on purpose */
				else
					fetch(s->selection_timestamp);
				break;
			case XFixesSelectionWindowDestroyNotify:
			case XFixesSelectionClientCloseNotify:
				/* The owner closed: we have its text, hand it out now */
				if (text && !owned_since) {
					XSetSelectionOwner(dpy, CLIPBOARD, win, s->timestamp);
					if (XGetSelectionOwner(dpy, CLIPBOARD) != win)
						XSetSelectionOwner(dpy, CLIPBOARD, win, CurrentTime);
					if (XGetSelectionOwner(dpy, CLIPBOARD) == win)
						owned_since = s->timestamp ? s->timestamp : 1;
				}
				break;
			}
		} else if (ev.type == SelectionRequest) {
			answer(&ev.xselectionrequest);
		} else if (ev.type == SelectionClear) {
			owned_since = 0;              /* someone copied something new */
		}
	}
}
