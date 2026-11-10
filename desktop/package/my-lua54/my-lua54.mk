################################################################################
#
# my-lua54: Lua 5.4 as a static library, only for WirePlumber. Buildroot's
# lua package can't be in the image together with LuaJIT (mpv's on-screen
# controls need LuaJIT; WirePlumber needs Lua 5.3 or 5.4), so this builds
# liblua.a into its own folder of the staging area with a lua5.4.pc that
# WirePlumber's build finds. Nothing goes into the image: WirePlumber has
# Lua built in.
#
################################################################################

MY_LUA54_VERSION = 5.4.8
MY_LUA54_SOURCE = lua-$(MY_LUA54_VERSION).tar.gz
MY_LUA54_SITE = https://www.lua.org/ftp
MY_LUA54_LICENSE = MIT
MY_LUA54_LICENSE_FILES = doc/readme.html
MY_LUA54_INSTALL_STAGING = YES
MY_LUA54_INSTALL_TARGET = NO

# -fPIC: it ends up inside WirePlumber's Lua module, a shared library
define MY_LUA54_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/src liblua.a \
		CC="$(TARGET_CC)" AR="$(TARGET_AR) rcu" RANLIB="$(TARGET_RANLIB)" \
		MYCFLAGS="$(TARGET_CFLAGS) -fPIC -DLUA_USE_LINUX"
endef

define MY_LUA54_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/src/liblua.a $(STAGING_DIR)/usr/lib/my-lua54/liblua.a
	mkdir -p $(STAGING_DIR)/usr/include/my-lua54
	for h in lua.h luaconf.h lualib.h lauxlib.h lua.hpp; do \
		$(INSTALL) -m 0644 $(@D)/src/$$h $(STAGING_DIR)/usr/include/my-lua54/ || exit 1; \
	done
	$(INSTALL) -D -m 0644 $(MY_LUA54_PKGDIR)/lua5.4.pc \
		$(STAGING_DIR)/usr/lib/pkgconfig/lua5.4.pc
endef

$(eval $(generic-package))
