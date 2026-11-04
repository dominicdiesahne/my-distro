################################################################################
#
# my-login: builds login/my-login.c (login screen) and login/my-power.c
#
################################################################################

MY_LOGIN_VERSION = 1.0
MY_LOGIN_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../login
MY_LOGIN_SITE_METHOD = local
MY_LOGIN_DEPENDENCIES = xlib_libX11 xlib_libXft xlib_libXrender imlib2 \
	libxcrypt host-pkgconf

define MY_LOGIN_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-login $(@D)/my-login.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 xft xrender imlib2 libxcrypt)
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-power $(@D)/my-power.c
endef

define MY_LOGIN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-login $(TARGET_DIR)/usr/bin/my-login
	$(INSTALL) -D -m 0755 $(@D)/my-power $(TARGET_DIR)/usr/bin/my-power
endef

# setuid root, so normal users can reboot (see my-power.c)
define MY_LOGIN_PERMISSIONS
	/usr/bin/my-power f 4755 0 0 - - - - -
endef

$(eval $(generic-package))
