/*
 * Speech for the login and lock screens (1.3.0). The screen reader
 * (my-reader) can't follow them: they are not GTK programs and grab the
 * keyboard. So they say what matters themselves, with eSpeak like
 * my-reader: the field that has the keyboard, the letters typed into the
 * user name (never the password's) and their messages. Something new
 * interrupts what is being said.
 *
 * Included by my-login.c and my-lock.c (one program each, no shared object).
 */
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t speech_pid;               /* espeak | aplay, in its own group */
static char speech_voice[8] = "en";
static const char *speech_device;      /* aplay -D DEVICE; NULL: ALSA's default */
static const char *speech_log;         /* also write what is said into this file */

static void speech_hush(void)
{
	if (speech_pid > 0) {
		kill(-speech_pid, SIGTERM);
		waitpid(speech_pid, NULL, 0);
	}
	speech_pid = 0;
}

/* The voice of a language: "de_DE.UTF-8" -> "de" */
static void speech_language(const char *lang)
{
	if (lang && isalpha((unsigned char)lang[0]) && isalpha((unsigned char)lang[1]))
		snprintf(speech_voice, sizeof speech_voice, "%c%c",
		         tolower((unsigned char)lang[0]), tolower((unsigned char)lang[1]));
}

static void speech_say(const char *text)
{
	pid_t pid;

	if (!text || !text[strspn(text, " \t\r\n")])
		return;
	if (speech_log) {
		FILE *f = fopen(speech_log, "a");

		if (f) {
			fprintf(f, "%s\n", text);
			fclose(f);
		}
	}
	speech_hush();
	pid = fork();
	if (pid == 0) {
		char cmd[160];
		FILE *p;

		signal(SIGCHLD, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		setsid();
		snprintf(cmd, sizeof cmd, "espeak --stdin --stdout -s 170 -v %s 2>/dev/null | "
		         "aplay -q%s%s 2>/dev/null", speech_voice,
		         speech_device ? " -D " : "", speech_device ? speech_device : "");
		if ((p = popen(cmd, "w"))) {
			fputs(text, p);
			fputc('\n', p);
			pclose(p);
		}
		_exit(0);
	}
	if (pid > 0)
		speech_pid = pid;
}
