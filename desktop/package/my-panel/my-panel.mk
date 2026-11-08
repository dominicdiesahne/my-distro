################################################################################
#
# my-panel: builds panel/my-panel.c (the taskbar), panel/my-screenshot.c,
# panel/my-clipd.c (keeps copied text), panel/my-zoom.c (magnifier) and
# panel/my-speak.c (reads the selected text aloud) with Buildroot's compiler
#
################################################################################

MY_PANEL_VERSION = 1.0
MY_PANEL_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../panel
MY_PANEL_SITE_METHOD = local
MY_PANEL_DEPENDENCIES = xlib_libX11 xlib_libXft xlib_libXi xlib_libXfixes imlib2 cairo \
	alsa-lib host-pkgconf

# settings.c and dial.c are shared with the Settings and Clock apps
MY_PANEL_SHARED = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../settings

define MY_PANEL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-I$(MY_PANEL_SHARED) -o $(@D)/my-panel $(@D)/my-panel.c \
		$(MY_PANEL_SHARED)/settings.c $(MY_PANEL_SHARED)/dial.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 xft xi imlib2 cairo-xlib alsa) -lm
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-screenshot $(@D)/my-screenshot.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 imlib2)
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-clipd $(@D)/my-clipd.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 xfixes)
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-zoom $(@D)/my-zoom.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 cairo-xlib)
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-speak $(@D)/my-speak.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11)
endef

define MY_PANEL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-panel $(TARGET_DIR)/usr/bin/my-panel
	$(INSTALL) -D -m 0755 $(@D)/my-screenshot $(TARGET_DIR)/usr/bin/my-screenshot
	$(INSTALL) -D -m 0755 $(@D)/my-clipd $(TARGET_DIR)/usr/bin/my-clipd
	$(INSTALL) -D -m 0755 $(@D)/my-zoom $(TARGET_DIR)/usr/bin/my-zoom
	$(INSTALL) -D -m 0755 $(@D)/my-speak $(TARGET_DIR)/usr/bin/my-speak
endef

$(eval $(generic-package))
