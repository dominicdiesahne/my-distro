################################################################################
#
# my-init: builds init/init.c with Buildroot's cross-compiler
#
################################################################################

MY_INIT_VERSION = 1.0
MY_INIT_SITE = $(BR2_EXTERNAL_MY_DISTRO_PATH)/../init
MY_INIT_SITE_METHOD = local

define MY_INIT_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(@D)/my-init $(@D)/init.c
endef

define MY_INIT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/my-init $(TARGET_DIR)/sbin/my-init
endef

$(eval $(generic-package))
