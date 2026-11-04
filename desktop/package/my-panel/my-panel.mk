################################################################################
#
# my-panel: builds panel/my-panel.c (the taskbar) with Buildroot's compiler
#
################################################################################

MY_PANEL_VERSION = 1.0
MY_PANEL_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../panel
MY_PANEL_SITE_METHOD = local
MY_PANEL_DEPENDENCIES = xlib_libX11 xlib_libXft imlib2 host-pkgconf

define MY_PANEL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-panel $(@D)/my-panel.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 xft imlib2)
endef

define MY_PANEL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-panel $(TARGET_DIR)/usr/bin/my-panel
endef

$(eval $(generic-package))
