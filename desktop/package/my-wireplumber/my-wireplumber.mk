################################################################################
#
# my-wireplumber: Buildroot's WirePlumber (package/wireplumber, same version
# and download) with Lua 5.4 built in from my-lua54. Buildroot's own package
# depends on the lua package, which can't be selected next to LuaJIT.
#
################################################################################

MY_WIREPLUMBER_VERSION = $(WIREPLUMBER_VERSION)
MY_WIREPLUMBER_SOURCE = $(WIREPLUMBER_SOURCE)
MY_WIREPLUMBER_SITE = $(WIREPLUMBER_SITE)
MY_WIREPLUMBER_DL_SUBDIR = wireplumber
MY_WIREPLUMBER_LICENSE = MIT
MY_WIREPLUMBER_LICENSE_FILES = LICENSE
MY_WIREPLUMBER_DEPENDENCIES = host-pkgconf pipewire libglib2 my-lua54 dbus

MY_WIREPLUMBER_CONF_OPTS = \
	-Ddoc=disabled \
	-Dtests=false \
	-Delogind=disabled \
	-Dsystem-lua=true \
	-Dsystem-lua-version=5.4 \
	-Dintrospection=disabled \
	-Dsystemd=disabled \
	-Dsystemd-system-service=false \
	-Dsystemd-user-service=false

$(eval $(meson-package))
