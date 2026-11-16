################################################################################
#
# my-podman-conf: podman's settings for my-distro (a Software app package,
# 0.10.0, installed with podman: "needs" in packages.catalog).
#
#   /etc/containers/storage.conf                       where images and
#       containers are kept, with the kernel's overlayfs: since Linux 5.13 it
#       also works inside a user namespace, so rootless containers need no
#       fuse-overlayfs (on ext4 and on the live system's tmpfs)
#   /etc/containers/containers.conf.d/60-my-distro.conf  no systemd here:
#       cgroupfs, events and logs into files; crun runs the containers
#
# registries.conf (docker.io for short names) and policy.json come from
# Buildroot's containers-image-config. The subordinate IDs of rootless
# containers (/etc/subuid, /etc/subgid) are made by my-subids in the image,
# the live system's no_pivot_root by /etc/init.d/S31containers.
#
# No build dependency on podman: then removing podman would refuse to go
# while this (which "needs" podman) is installed.
#
################################################################################

MY_PODMAN_CONF_VERSION = 1
MY_PODMAN_CONF_SITE = $(MY_PODMAN_CONF_PKGDIR)/files
MY_PODMAN_CONF_SITE_METHOD = local
MY_PODMAN_CONF_LICENSE = MIT

define MY_PODMAN_CONF_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/storage.conf \
		$(TARGET_DIR)/etc/containers/storage.conf
	$(INSTALL) -D -m 0644 $(@D)/60-my-distro.conf \
		$(TARGET_DIR)/etc/containers/containers.conf.d/60-my-distro.conf
endef

$(eval $(generic-package))
