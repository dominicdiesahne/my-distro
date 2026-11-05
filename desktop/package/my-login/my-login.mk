################################################################################
#
# my-login: builds login/my-login.c (login screen), login/my-power.c and
# login/my-admin.c (opens "Users and system" as root)
#
################################################################################

MY_LOGIN_VERSION = 1.0
MY_LOGIN_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../login
MY_LOGIN_SITE_METHOD = local
MY_LOGIN_DEPENDENCIES = xlib_libX11 xlib_libXft xlib_libXrender imlib2 \
	libxcrypt host-pkgconf

# settings.c: the colors and wallpaper chosen in the Settings app
MY_LOGIN_SHARED = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../settings

define MY_LOGIN_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-I$(MY_LOGIN_SHARED) -o $(@D)/my-login $(@D)/my-login.c \
		$(MY_LOGIN_SHARED)/settings.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 xft xrender imlib2 libxcrypt)
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-power $(@D)/my-power.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-admin $(@D)/my-admin.c \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs libxcrypt)
endef

define MY_LOGIN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-login $(TARGET_DIR)/usr/bin/my-login
	$(INSTALL) -D -m 0755 $(@D)/my-power $(TARGET_DIR)/usr/bin/my-power
	$(INSTALL) -D -m 0755 $(@D)/my-admin $(TARGET_DIR)/usr/bin/my-admin
endef

# setuid root, so normal users can reboot (see my-power.c) and open
# "Users and system" with the root password (see my-admin.c)
define MY_LOGIN_PERMISSIONS
	/usr/bin/my-power f 4755 0 0 - - - - -
	/usr/bin/my-admin f 4755 0 0 - - - - -
endef

$(eval $(generic-package))
