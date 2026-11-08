/*
 * i18n.h - translations of my-distro's own programs (GNU gettext, in glibc).
 *
 * Every text shown to people is written in English inside _("..."). At run
 * time gettext() looks it up in the translation for the language in LANG,
 * e.g. /usr/share/locale/de/LC_MESSAGES/my-distro.mo, made while building
 * from po/de.po (all programs share that one file). Missing translations
 * stay English. N_("...") only marks a text for translation, for tables
 * that C needs as constants; _() translates it where it is shown.
 *
 * The language: ~/.xinitrc sets LANG for the session from Settings,
 * Language (/usr/lib/my-distro/session-env). Programs without a session (the
 * login screen, the setup) use the system's language, chosen in the setup:
 * /etc/my-distro/language.
 *
 * Add a language: copy po/de.po to po/XX.po, translate the msgstr lines,
 * add "XX" to LINGUAS in po/Makefile and the language to LANGUAGES below.
 */
#ifndef MY_I18N_H
#define MY_I18N_H

#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _(s)  gettext(s)
#define N_(s) (s)

#define I18N_DOMAIN     "my-distro"
#define I18N_LOCALEDIR  "/usr/share/locale"
#define SYSTEM_LANGUAGE "/etc/my-distro/language"

/* The languages my-distro is translated into: LANG value and name */
#define LANGUAGES { "en_US.UTF-8", "English" }, { "de_DE.UTF-8", "Deutsch" }, \
                  { "de_AT.UTF-8", "Deutsch (Österreich)" }

/* Turns on translations for the language in LANG (call before gtk_init) */
static inline void i18n_init(void)
{
	setlocale(LC_ALL, "");
	bindtextdomain(I18N_DOMAIN, I18N_LOCALEDIR);
	bind_textdomain_codeset(I18N_DOMAIN, "UTF-8");
	textdomain(I18N_DOMAIN);
}

/* The system's language from /etc/my-distro/language ("de_DE.UTF-8"),
 * English when it is not set */
static inline void i18n_system_language(char *buf, size_t len)
{
	FILE *f = fopen(SYSTEM_LANGUAGE, "r");

	snprintf(buf, len, "en_US.UTF-8");
	if (f) {
		char line[64];

		if (fgets(line, sizeof line, f) && strspn(line, "abcdefghijklmnopqrstuvwxyz"
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZ_.-@0123456789") >= 2) {
			line[strspn(line, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_.-@0123456789")] = '\0';
			snprintf(buf, len, "%s", line);
		}
		fclose(f);
	}
}

/* For programs outside a session: LANG = the system's language */
static inline void i18n_init_system(void)
{
	char lang[64];

	i18n_system_language(lang, sizeof lang);
	setenv("LANG", lang, 1);
	unsetenv("LANGUAGE");
	unsetenv("LC_ALL");
	unsetenv("LC_MESSAGES");
	i18n_init();
}

#endif
