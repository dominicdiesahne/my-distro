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

# Xorg's drivers use functions of modules that Xorg loads after them
# (modesetting: libgbm, since it draws with the graphics chip in 0.9.5;
# fbdev: the fbdevhw module). Buildroot links everything with "bind now"
# (full RELRO), which wants every function found while loading, so Xorg
# found no driver and the screen stayed black. Lazy binding for these two.
XSERVER_XORG_SERVER_CONF_ENV += LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,lazy"
XDRIVER_XF86_VIDEO_FBDEV_CONF_ENV += LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,lazy"

# Mesa refuses to configure unless its C++ RTTI setting matches its LLVM's.
# Buildroot decides from BR2_PACKAGE_LLVM_RTTI, which Intel's drivers
# (0.9.7) switch on; but the build machine's LLVM (host-mesa3d, which
# compiles Intel's shaders) is built without RTTI, and an LLVM built before
# that option was on has none either. So ask each LLVM itself (when Mesa is
# configured, after LLVM is built: these are expanded only then).
HOST_MESA3D_CONF_OPTS += -Dcpp_rtti=$(if $(filter YES,$(shell \
	$(HOST_DIR)/bin/llvm-config --has-rtti 2>/dev/null)),true,false)
MESA3D_CONF_OPTS += -Dcpp_rtti=$(if $(filter YES,$(shell \
	$(STAGING_DIR)/usr/bin/llvm-config --has-rtti 2>/dev/null)),true,false)

# PrBoom (Doom, a package of the Software app) is from 2008 and defines its
# own "false" and "true", which C23 (GCC 15's default) made keywords: build
# it as C17.
PRBOOM_CONF_ENV += CFLAGS="$(TARGET_CFLAGS) -std=gnu17"
