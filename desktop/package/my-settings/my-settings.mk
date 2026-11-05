################################################################################
#
# my-settings: the Settings app and the Clock app (settings/). settings.c and
# dial.c are also compiled into my-panel and my-login.
#
################################################################################

MY_SETTINGS_VERSION = 1.0
MY_SETTINGS_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../settings
MY_SETTINGS_SITE_METHOD = local
MY_SETTINGS_DEPENDENCIES = libgtk3 cairo host-pkgconf

MY_SETTINGS_CC = $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra
MY_SETTINGS_GTK = $$($(PKG_CONFIG_HOST_BINARY) --cflags --libs gtk+-3.0)

define MY_SETTINGS_BUILD_CMDS
	$(MY_SETTINGS_CC) -o $(@D)/my-settings $(@D)/my-settings.c \
		$(@D)/settings.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-clock $(@D)/my-clock.c $(@D)/dial.c \
		$(@D)/settings.c $(MY_SETTINGS_GTK) -lm
endef

define MY_SETTINGS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-settings $(TARGET_DIR)/usr/bin/my-settings
	$(INSTALL) -D -m 0755 $(@D)/my-clock $(TARGET_DIR)/usr/bin/my-clock
endef

$(eval $(generic-package))
