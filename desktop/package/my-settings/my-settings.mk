################################################################################
#
# my-settings: the Settings app, the Clock app, the setup / "Users and
# system" window (my-setup) and the Wi-Fi window (my-wifi) from settings/. settings.c and
# dial.c are also compiled into my-panel and my-login.
#
################################################################################

MY_SETTINGS_VERSION = 1.0
MY_SETTINGS_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../settings
MY_SETTINGS_SITE_METHOD = local
MY_SETTINGS_DEPENDENCIES = libgtk3 cairo libxcrypt host-pkgconf

MY_SETTINGS_CC = $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra
MY_SETTINGS_GTK = $$($(PKG_CONFIG_HOST_BINARY) --cflags --libs gtk+-3.0)

define MY_SETTINGS_BUILD_CMDS
	$(MY_SETTINGS_CC) -o $(@D)/my-settings $(@D)/my-settings.c \
		$(@D)/settings.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-clock $(@D)/my-clock.c $(@D)/dial.c \
		$(@D)/settings.c $(MY_SETTINGS_GTK) -lm
	$(MY_SETTINGS_CC) -o $(@D)/my-setup $(@D)/my-setup.c $(@D)/settings.c \
		$(MY_SETTINGS_GTK) $$($(PKG_CONFIG_HOST_BINARY) --cflags --libs libxcrypt)
	$(MY_SETTINGS_CC) -o $(@D)/my-wifi $(@D)/my-wifi.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-notifyd $(@D)/my-notifyd.c $(@D)/settings.c \
		$(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-notify $(@D)/my-notify.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs gio-2.0)
endef

define MY_SETTINGS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-settings $(TARGET_DIR)/usr/bin/my-settings
	$(INSTALL) -D -m 0755 $(@D)/my-clock $(TARGET_DIR)/usr/bin/my-clock
	$(INSTALL) -D -m 0755 $(@D)/my-setup $(TARGET_DIR)/usr/bin/my-setup
	$(INSTALL) -D -m 0755 $(@D)/my-wifi $(TARGET_DIR)/usr/bin/my-wifi
	$(INSTALL) -D -m 0755 $(@D)/my-notifyd $(TARGET_DIR)/usr/bin/my-notifyd
	$(INSTALL) -D -m 0755 $(@D)/my-notify $(TARGET_DIR)/usr/bin/my-notify
	ln -sf my-notify $(TARGET_DIR)/usr/bin/notify-send
endef

$(eval $(generic-package))
