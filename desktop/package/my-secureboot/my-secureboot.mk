################################################################################
#
# my-secureboot: what a PC with Secure Boot on needs to start my-distro.
#
# UEFI firmware with Secure Boot only starts programs signed by Microsoft.
# Microsoft signs "shim" for Linux distributions; shim then starts programs
# signed with keys it knows, including the "Machine Owner Keys" (MOK) that
# the owner of the PC enrolled once with MokManager. my-distro uses
# Debian's shim and MokManager unchanged (the .deb files from the official
# Debian archive; the checksums in my-secureboot.hash are the ones in
# Debian's signed package list, see desktop/secureboot/README), and signs
# its own GRUB and kernel with my-distro's Secure Boot key. The certificate
# of that key (desktop/secureboot/my-distro-secureboot.crt) is what the
# owner enrolls; the private key never leaves the build machine.
#
# Installed to /usr/lib/my-distro/efi:
#   BOOTX64.EFI   shim (the firmware starts it), which starts grubx64.efi
#   mmx64.efi     MokManager (enrolling a key), started by shim when needed
#   my-distro-secureboot.cer  the certificate, for "Enroll key from disk"
# grubx64.efi (GRUB with all its modules built in, signed when the key is
# there) is made by desktop/post-build.sh. The ISO's UEFI boot image and
# my-install take all of them from there.
#
################################################################################

MY_SECUREBOOT_VERSION = 16.1
MY_SECUREBOOT_SITE = https://deb.debian.org/debian/pool/main/s/shim-signed
MY_SECUREBOOT_SOURCE = shim-signed_1.51~1+deb13u1+16.1-2~deb13u1_amd64.deb
MY_SECUREBOOT_HELPERS = shim-helpers-amd64-signed_1+16.1+2~deb13u1_amd64.deb
MY_SECUREBOOT_EXTRA_DOWNLOADS = \
	https://deb.debian.org/debian/pool/main/s/shim-helpers-amd64-signed/$(MY_SECUREBOOT_HELPERS)
MY_SECUREBOOT_LICENSE = BSD-2-Clause (shim)
MY_SECUREBOOT_CERT = $(BR2_EXTERNAL_MY_DISTRO_PATH)/secureboot/my-distro-secureboot.crt
# osslsigncode signs GRUB and the kernel (post-build.sh, make desktop-secureboot)
MY_SECUREBOOT_DEPENDENCIES = host-osslsigncode host-openssl

# .deb files are "ar" archives with the files in data.tar.xz
define MY_SECUREBOOT_EXTRACT_CMDS
	mkdir -p $(@D)/shim $(@D)/helpers
	cd $(@D)/shim && ar x $(MY_SECUREBOOT_DL_DIR)/$(MY_SECUREBOOT_SOURCE) && tar -xf data.tar.*
	cd $(@D)/helpers && ar x $(MY_SECUREBOOT_DL_DIR)/$(MY_SECUREBOOT_HELPERS) && tar -xf data.tar.*
endef

define MY_SECUREBOOT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $$(readlink -f $(@D)/shim/usr/lib/shim/shimx64.efi.signed) \
		$(TARGET_DIR)/usr/lib/my-distro/efi/BOOTX64.EFI
	$(INSTALL) -D -m 0644 $(@D)/helpers/usr/lib/shim/mmx64.efi.signed \
		$(TARGET_DIR)/usr/lib/my-distro/efi/mmx64.efi
	$(HOST_DIR)/bin/openssl x509 -in $(MY_SECUREBOOT_CERT) -outform der \
		-out $(TARGET_DIR)/usr/lib/my-distro/efi/my-distro-secureboot.cer
endef

$(eval $(generic-package))
