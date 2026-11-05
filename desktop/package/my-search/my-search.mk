################################################################################
#
# my-search: semantic file search (search/). The model comes from the
# package my-search-model.
#
################################################################################

MY_SEARCH_VERSION = 1.0
MY_SEARCH_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../search
MY_SEARCH_SITE_METHOD = local
MY_SEARCH_DEPENDENCIES = llama-cpp my-search-model xlib_libX11 xlib_libXft \
	host-pkgconf

# settings.c: the window uses the colors chosen in the Settings app
MY_SEARCH_SHARED = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../settings

MY_SEARCH_CC = $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra
MY_SEARCH_LLAMA = $$($(PKG_CONFIG_HOST_BINARY) --cflags --libs llama) -lm

define MY_SEARCH_BUILD_CMDS
	$(MY_SEARCH_CC) -o $(@D)/my-indexd $(@D)/my-indexd.c $(@D)/index.c \
		$(@D)/embed.c $(MY_SEARCH_LLAMA)
	$(MY_SEARCH_CC) -o $(@D)/my-search $(@D)/my-search.c $(@D)/index.c \
		$(@D)/embed.c $(MY_SEARCH_LLAMA)
	$(MY_SEARCH_CC) -I$(MY_SEARCH_SHARED) -o $(@D)/my-search-gui $(@D)/my-search-gui.c \
		$(@D)/index.c $(MY_SEARCH_SHARED)/settings.c \
		$(@D)/embed.c $(MY_SEARCH_LLAMA) \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags --libs x11 xft)
endef

define MY_SEARCH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-indexd $(TARGET_DIR)/usr/bin/my-indexd
	$(INSTALL) -D -m 0755 $(@D)/my-search $(TARGET_DIR)/usr/bin/my-search
	$(INSTALL) -D -m 0755 $(@D)/my-search-gui $(TARGET_DIR)/usr/bin/my-search-gui
endef

$(eval $(generic-package))
