################################################################################
#
# my-settings: the Settings app, the Clock app, the setup / "Users and
# system" window (my-setup), the Wi-Fi window (my-wifi), the VPN window
# (my-vpn), the log viewer (my-logs), the Software app (my-software) and
# its file chooser for the Start menu (my-pick-file), and since 0.9.5 the
# Bluetooth, Printers, Disks and Backup windows from settings/. settings.c
# and dial.c are also compiled into my-panel and my-login.
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
	$(MY_SETTINGS_CC) -o $(@D)/my-vpn $(@D)/my-vpn.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-logs $(@D)/my-logs.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-software $(@D)/my-software.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-pick-file $(@D)/my-pick-file.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-notifyd $(@D)/my-notifyd.c $(@D)/settings.c \
		$(MY_SETTINGS_GTK) $$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11)
	$(MY_SETTINGS_CC) -o $(@D)/my-notify $(@D)/my-notify.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs gio-2.0)
	$(MY_SETTINGS_CC) -o $(@D)/my-bluetooth $(@D)/my-bluetooth.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-printers $(@D)/my-printers.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-disks $(@D)/my-disks.c $(MY_SETTINGS_GTK)
	$(MY_SETTINGS_CC) -o $(@D)/my-backup-gui $(@D)/my-backup-gui.c $(MY_SETTINGS_GTK)
endef

define MY_SETTINGS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-settings $(TARGET_DIR)/usr/bin/my-settings
	$(INSTALL) -D -m 0755 $(@D)/my-clock $(TARGET_DIR)/usr/bin/my-clock
	$(INSTALL) -D -m 0755 $(@D)/my-setup $(TARGET_DIR)/usr/bin/my-setup
	$(INSTALL) -D -m 0755 $(@D)/my-wifi $(TARGET_DIR)/usr/bin/my-wifi
	$(INSTALL) -D -m 0755 $(@D)/my-vpn $(TARGET_DIR)/usr/bin/my-vpn
	$(INSTALL) -D -m 0755 $(@D)/my-logs $(TARGET_DIR)/usr/bin/my-logs
	$(INSTALL) -D -m 0755 $(@D)/my-software $(TARGET_DIR)/usr/bin/my-software
	$(INSTALL) -D -m 0755 $(@D)/my-pick-file $(TARGET_DIR)/usr/bin/my-pick-file
	$(INSTALL) -D -m 0755 $(@D)/my-notifyd $(TARGET_DIR)/usr/bin/my-notifyd
	$(INSTALL) -D -m 0755 $(@D)/my-notify $(TARGET_DIR)/usr/bin/my-notify
	ln -sf my-notify $(TARGET_DIR)/usr/bin/notify-send
	$(INSTALL) -D -m 0755 $(@D)/my-bluetooth $(TARGET_DIR)/usr/bin/my-bluetooth
	$(INSTALL) -D -m 0755 $(@D)/my-printers $(TARGET_DIR)/usr/bin/my-printers
	$(INSTALL) -D -m 0755 $(@D)/my-disks $(TARGET_DIR)/usr/bin/my-disks
	$(INSTALL) -D -m 0755 $(@D)/my-backup-gui $(TARGET_DIR)/usr/bin/my-backup-gui
endef

$(eval $(generic-package))
