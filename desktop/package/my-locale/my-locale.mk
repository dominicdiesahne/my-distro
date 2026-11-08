################################################################################
#
# my-locale: the translations of my-distro's own programs. Every po/LANG.po
# (texts in English and their translation) is compiled with msgfmt into
# /usr/share/locale/LANG/LC_MESSAGES/my-distro.mo, which gettext reads at
# run time (see settings/i18n.h).
#
################################################################################

MY_LOCALE_VERSION = 1.0
MY_LOCALE_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../po
MY_LOCALE_SITE_METHOD = local
MY_LOCALE_DEPENDENCIES = host-gettext

define MY_LOCALE_BUILD_CMDS
	for po in $(@D)/*.po; do \
		$(HOST_DIR)/bin/msgfmt --check -o $${po%.po}.mo $$po || exit 1; \
	done
endef

define MY_LOCALE_INSTALL_TARGET_CMDS
	for mo in $(@D)/*.mo; do \
		lang=$$(basename $$mo .mo); \
		$(INSTALL) -D -m 0644 $$mo \
			$(TARGET_DIR)/usr/share/locale/$$lang/LC_MESSAGES/my-distro.mo; \
	done
endef

$(eval $(generic-package))
