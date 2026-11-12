################################################################################
#
# tinycc: the Tiny C Compiler, so C programs can be compiled on my-distro
# itself (Buildroot has no GCC for the target). A Software app package.
#
# A compiler needs more than itself: the C library's headers and its start
# files (crt1.o, ...), which Buildroot leaves out of the image. They come
# from the cross-compiler's sysroot (only glibc's and the kernel's headers)
# and go with tcc into /usr/share/tcc: Buildroot deletes every *.a in /lib
# and /usr/lib of the image, and tcc needs libtcc1.a and libc_nonshared.a.
#   /usr/share/tcc/include     tcc's own headers (stdarg.h, stddef.h, ...)
#   /usr/share/tcc/sysinclude  glibc's and Linux's headers
#   /usr/share/tcc/lib         crt1.o, crti.o, crtn.o, libc_nonshared.a and
#                              small linker scripts for -lc and -lm
#
################################################################################

# The development branch ("mob", as of 2026-11-10): the last release
# (0.9.27) is from 2017 and doesn't know newer glibc headers
TINYCC_VERSION = aade7bafb59641660bc9a0e21ad0a9a4a1ed346e
TINYCC_SITE = https://repo.or.cz/tinycc.git
TINYCC_SITE_METHOD = git
TINYCC_LICENSE = LGPL-2.1+
TINYCC_LICENSE_FILES = COPYING

TINYCC_DIR = /usr/share/tcc

# {B} is tcc's own folder (TINYCC_DIR). tcc's predefined macros are read
# from tccdefs.h at run time (config-predefs=no): making them while building
# would run the new compiler here.
# No bounds checker (bcheck.o is compiled with tcc itself, option -bt, which
# the cross compiler does not know).
define TINYCC_CONFIGURE_CMDS
	cd $(@D) && $(TARGET_MAKE_ENV) ./configure \
		--prefix=/usr --tccdir=$(TINYCC_DIR) \
		--cc="$(TARGET_CC)" --ar="$(TARGET_AR)" --cpu=x86_64 \
		--triplet= --config-predefs=no --config-bcheck=no \
		--sysincludepaths="{B}/include:{B}/sysinclude" \
		--libpaths="{B}/lib:{B}:/usr/lib:/lib" \
		--crtprefix="{B}/lib" \
		--elfinterp=/lib/ld-linux-x86-64.so.2
endef

# libtcc1.a (helpers that tcc's code calls) is compiled with the cross
# compiler instead of the new tcc, which can't run on the build machine
define TINYCC_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) tcc
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) libtcc1.a XCC="$(TARGET_CC)" XAR="$(TARGET_AR)"
endef

define TINYCC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tcc $(TARGET_DIR)/usr/bin/tcc
	$(INSTALL) -D -m 0644 $(@D)/libtcc1.a $(TARGET_DIR)$(TINYCC_DIR)/libtcc1.a
	mkdir -p $(TARGET_DIR)$(TINYCC_DIR)/include $(TARGET_DIR)$(TINYCC_DIR)/sysinclude \
		$(TARGET_DIR)$(TINYCC_DIR)/lib
	cp -a $(@D)/include/. $(TARGET_DIR)$(TINYCC_DIR)/include/
	S=$$($(TARGET_CC) -print-sysroot) && \
		cp -a $$S/usr/include/. $(TARGET_DIR)$(TINYCC_DIR)/sysinclude/ && \
		cp $$S/usr/lib/crt1.o $$S/usr/lib/crti.o $$S/usr/lib/crtn.o \
			$$S/usr/lib/libc_nonshared.a $(TARGET_DIR)$(TINYCC_DIR)/lib/
	printf '%s\n' "GROUP ( /lib/libc.so.6 $(TINYCC_DIR)/lib/libc_nonshared.a AS_NEEDED ( /lib/ld-linux-x86-64.so.2 ) )" \
		>$(TARGET_DIR)$(TINYCC_DIR)/lib/libc.so
	printf '%s\n' "INPUT ( /lib/libm.so.6 )" >$(TARGET_DIR)$(TINYCC_DIR)/lib/libm.so
endef

$(eval $(generic-package))
