include $(sort $(wildcard $(BR2_EXTERNAL_MY_DISTRO_PATH)/package/*/*.mk))

# libfm and PCManFM (the file manager) are from 2016-2018. GCC 14 turned a
# warning about mixing up const pointers into an error, so turn it back into
# a warning for these two.
LIBFM_CONF_ENV += CFLAGS="$(TARGET_CFLAGS) -Wno-error=incompatible-pointer-types"
PCMANFM_CONF_ENV += CFLAGS="$(TARGET_CFLAGS) -Wno-error=incompatible-pointer-types"

# NetSurf packs its window icon (netsurf.xpm) into the program while building.
# Newer gdk-pixbuf can't read XPM images any more, so put the PNG version of
# the same icon in its place. gdk-pixbuf detects the format from the content.
define NETSURF_ICON_AS_PNG
	cp $(@D)/netsurf/frontends/gtk/res/netsurf.png \
		$(@D)/netsurf/frontends/gtk/res/netsurf.xpm
endef
NETSURF_POST_PATCH_HOOKS += NETSURF_ICON_AS_PNG
