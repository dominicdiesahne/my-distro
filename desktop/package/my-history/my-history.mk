################################################################################
#
# my-history: builds history/ (my-historyd, my-undo, my-history)
#
################################################################################

MY_HISTORY_VERSION = 1.0
MY_HISTORY_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../history
MY_HISTORY_SITE_METHOD = local
MY_HISTORY_DEPENDENCIES = xlib_libX11 xlib_libXft host-pkgconf

MY_HISTORY_CC = $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra

define MY_HISTORY_BUILD_CMDS
	$(MY_HISTORY_CC) -o $(@D)/my-historyd $(@D)/my-historyd.c $(@D)/history.c
	$(MY_HISTORY_CC) -o $(@D)/my-undo $(@D)/my-undo.c $(@D)/history.c
	$(MY_HISTORY_CC) -o $(@D)/my-history $(@D)/my-history.c $(@D)/history.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 xft)
endef

define MY_HISTORY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-historyd $(TARGET_DIR)/usr/sbin/my-historyd
	$(INSTALL) -D -m 0755 $(@D)/my-undo $(TARGET_DIR)/usr/bin/my-undo
	$(INSTALL) -D -m 0755 $(@D)/my-history $(TARGET_DIR)/usr/bin/my-history
endef

$(eval $(generic-package))
