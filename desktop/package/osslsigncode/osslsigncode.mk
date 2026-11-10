################################################################################
#
# osslsigncode (build machine only): signs UEFI programs (PE files) with
# Authenticode signatures, the kind shim and UEFI firmware check. Used to
# sign GRUB, the kernel and fwupd's UEFI program with my-distro's Secure
# Boot key (desktop/post-build.sh, make desktop-secureboot).
#
################################################################################

OSSLSIGNCODE_VERSION = 2.9
OSSLSIGNCODE_SITE = $(call github,mtrojnar,osslsigncode,$(OSSLSIGNCODE_VERSION))
OSSLSIGNCODE_LICENSE = GPL-3.0+
OSSLSIGNCODE_LICENSE_FILES = COPYING.txt
HOST_OSSLSIGNCODE_DEPENDENCIES = host-openssl host-zlib
# No downloading of timestamps (curl) and no tests (Python)
HOST_OSSLSIGNCODE_CONF_OPTS = \
	-DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON \
	-DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON

$(eval $(host-cmake-package))
