/*
 * my-pick-file: asks for a file, then starts a program with it.
 *
 *   my-pick-file [--title TITLE] [--mime TYPE]... PROGRAM [ARGS...]
 *
 * For Start menu entries of programs that need a file to open (a PDF viewer,
 * an image viewer from the Software app): the file chooser starts in your
 * Documents or Pictures folder and shows only the files of those types (if
 * --mime is given), then runs PROGRAM ARGS... FILE. Cancel starts nothing.
 */
#include <gtk/gtk.h>

#include "i18n.h"
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *title = "Open";
	GtkFileFilter *filter = NULL;
	GtkWidget *d;
	char *file = NULL, **run;
	const char *folder;
	int i = 1, n = 0, pictures = 0;

	i18n_init();
	gtk_init(&argc, &argv);
	for (; i + 1 < argc && strncmp(argv[i], "--", 2) == 0; i += 2) {
		if (strcmp(argv[i], "--title") == 0) {
			title = argv[i + 1];
		} else if (strcmp(argv[i], "--mime") == 0) {
			if (!filter)
				filter = gtk_file_filter_new();
			gtk_file_filter_add_mime_type(filter, argv[i + 1]);
			pictures |= strncmp(argv[i + 1], "image/", 6) == 0;
		}
	}
	if (i >= argc) {
		fprintf(stderr, "usage: my-pick-file [--title T] [--mime TYPE]... PROGRAM [ARGS...]\n");
		return 1;
	}

	d = gtk_file_chooser_dialog_new(title, NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
		_("Cancel"), GTK_RESPONSE_CANCEL, _("Open"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_window_set_icon_name(GTK_WINDOW(d), "document-open");
	if (filter) {
		GtkFileFilter *all = gtk_file_filter_new();

		gtk_file_filter_set_name(filter, "Files it opens");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), filter);
		gtk_file_filter_set_name(all, "All files");
		gtk_file_filter_add_pattern(all, "*");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), all);
	}
	/* pictures in Pictures, everything else in Documents */
	folder = g_get_user_special_dir(pictures ? G_USER_DIRECTORY_PICTURES
	                                         : G_USER_DIRECTORY_DOCUMENTS);
	if (folder)
		gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), folder);
	if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT)
		file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
	gtk_widget_destroy(d);
	while (gtk_events_pending())
		gtk_main_iteration();
	if (!file)
		return 0;

	/* PROGRAM ARGS... FILE */
	run = g_new0(char *, argc - i + 2);
	for (; i < argc; i++)
		run[n++] = argv[i];
	run[n] = file;
	execvp(run[0], run);
	fprintf(stderr, "my-pick-file: can't start %s\n", run[0]);
	return 1;
}
