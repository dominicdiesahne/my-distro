# my-distro: a tiny Linux distro = Linux kernel + BusyBox + our own init in C.
#
# Run inside WSL (Kali):
#   make               build the minimal system: kernel, BusyBox, init, initramfs
#   make run           boot it in QEMU in this terminal (quit: Ctrl-A, then X)
#   make desktop       build the desktop system with Buildroot (Xorg + Openbox),
#                      the bootable ISO out/my-distro.iso and the update package
#   make release-files sign the update package for a GitHub release
#   make secureboot-key     make my-distro's Secure Boot key (once, outside the repository)
#   make desktop-secureboot the same as make desktop, with GRUB and the kernel
#                      signed with that key, so it starts with Secure Boot on
#   make packages      build the extra programs of the Software app (my-pkg):
#                      out/packages/, one .tar.xz each and the package list
#   make release-packages  sign that package list for the same release
#   make run-desktop   boot the desktop in a QEMU window
#   make run-iso       boot the ISO in a QEMU window
#   make run-install   boot the ISO with an empty disk, to test the installer
#   make run-disk      boot the system installed on that disk
#   make clean         rebuild only init + initramfs next time
#
# Recipes start with '>' instead of a tab (see .RECIPEPREFIX).

.RECIPEPREFIX := >
SHELL := /bin/bash

KERNEL_VERSION    := 6.18.60
BUSYBOX_VERSION   := 1.37.0
BUILDROOT_VERSION := 2026.08.1

# Build in the Linux filesystem, not under /mnt/c: Windows folders are slow
# and case-insensitive, which breaks the kernel source tree.
BUILD ?= $(HOME)/my-distro-build
JOBS  ?= $(shell nproc)
# make desktop BUILD=...: make would hand BUILD down to every package's own
# make (on their command line and in the environment), and NetSurf's build
# has a variable of that name. Keep it here.
MAKEOVERRIDES := $(filter-out BUILD=%,$(MAKEOVERRIDES))
unexport BUILD

KERNEL_TAR  := $(BUILD)/linux-$(KERNEL_VERSION).tar.xz
KERNEL_DIR  := $(BUILD)/linux-$(KERNEL_VERSION)
BZIMAGE     := $(KERNEL_DIR)/arch/x86/boot/bzImage
BUSYBOX_TAR := $(BUILD)/busybox-$(BUSYBOX_VERSION).tar.bz2
BUSYBOX_DIR := $(BUILD)/busybox-$(BUSYBOX_VERSION)
BUSYBOX_BIN := $(BUSYBOX_DIR)/busybox
INIT_BIN    := $(BUILD)/init
ROOTFS      := $(BUILD)/rootfs
INITRAMFS   := $(BUILD)/initramfs.cpio.gz

BR_TAR := $(BUILD)/buildroot-$(BUILDROOT_VERSION).tar.xz
BR_DIR := $(BUILD)/buildroot-$(BUILDROOT_VERSION)
BR_SRC := $(BUILD)/desktop-src
BR_OUT := $(BUILD)/desktop-out
# Buildroot refuses to run when PATH contains spaces, and WSL adds Windows
# folders like "C:\Program Files" to PATH, so give it a clean one.
# MY_SB_KEY: the Secure Boot key that desktop/post-build.sh signs GRUB and
# the kernel with (only make desktop-secureboot sets it)
BR_MAKE := env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
	MY_SB_KEY=$(MY_SB_KEY) \
	$(MAKE) -C $(BR_DIR) O=$(BR_OUT) BR2_EXTERNAL=$(BR_SRC)/desktop

# Use hardware acceleration if this user can open /dev/kvm.
KVM := $(shell [ -w /dev/kvm ] && echo -enable-kvm)

.PHONY: all run desktop release-files packages release-packages run-desktop run-iso \
	run-install run-disk clean distclean secureboot-key desktop-secureboot

UPDATE      := out/my-distro-update.tar.xz
# The files Buildroot put on its ISO (kernel, initrd, GRUB, boot menu)
ISO_FILES   := $(BR_OUT)/build/buildroot-fs/rootfs.iso9660.tmp
SIGNING_KEY ?= $(HOME)/.config/my-distro/update-signing-key.pem
# Secure Boot (make secureboot-key, make desktop-secureboot): the private
# key stays on this machine, outside the repository (SB_BACKUP=folder: a
# second copy); only the certificate is in the repository.
SB_KEY_DIR ?= $(HOME)/my-distro-secureboot-key
SB_KEY     := $(SB_KEY_DIR)/my-distro-secureboot.key
SB_CERT    := desktop/secureboot/my-distro-secureboot.crt
SB_BACKUP  ?=
MY_SB_KEY  ?=

# The Software app's packages (make packages)
PKG_OUT   := out/packages
PKG_INDEX := $(PKG_OUT)/my-pkg-index.json
PKG_WORK  := $(BR_OUT)/my-pkg-work
PKG_TOOL  := python3 desktop/make-packages.py
# make packages PKG_JOBS=4: fewer compilers at once (big C++ programs need ~1 GB each)
PKG_JOBS  ?=
# Takes what `make packages` built out of the build tree again, so the image
# is built without it; the image packages whose files it had overwritten are
# installed again.
PKG_CLEAN = r=$$($(PKG_TOOL) clean $(BR_OUT) $(UPDATE)) || exit 1; \
	for p in $$r; do $(BR_MAKE) $$p-reinstall || exit 1; done

all: $(BZIMAGE) $(INITRAMFS)

run: all
> qemu-system-x86_64 $(KVM) -m 512M \
>   -kernel $(BZIMAGE) -initrd $(INITRAMFS) \
>   -append "console=ttyS0" -nographic

# --- Linux kernel ----------------------------------------------------------

$(KERNEL_TAR):
> mkdir -p $(BUILD)
> wget -q --show-progress -O $@.part https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$(KERNEL_VERSION).tar.xz
> mv $@.part $@

$(KERNEL_DIR)/Makefile: $(KERNEL_TAR)
> tar -xf $< -C $(BUILD)
> touch $@

# defconfig = the standard x86_64 setup; config/kernel.fragment adds our changes.
$(BZIMAGE): $(KERNEL_DIR)/Makefile config/kernel.fragment
> cp config/kernel.fragment $(BUILD)/kernel.fragment
> $(MAKE) -C $(KERNEL_DIR) defconfig
> cd $(KERNEL_DIR) && scripts/kconfig/merge_config.sh -m .config $(BUILD)/kernel.fragment
> $(MAKE) -C $(KERNEL_DIR) olddefconfig
> $(MAKE) -C $(KERNEL_DIR) -j$(JOBS) bzImage

# --- BusyBox (shell + ~300 standard commands in one binary) ----------------

$(BUSYBOX_TAR):
> mkdir -p $(BUILD)
> wget -q --show-progress -O $@.part https://busybox.net/downloads/busybox-$(BUSYBOX_VERSION).tar.bz2
> mv $@.part $@

# Static build, so the initramfs needs no shared libraries.
# CONFIG_TC is off because it doesn't compile against new kernel headers.
$(BUSYBOX_BIN): $(BUSYBOX_TAR)
> tar -xf $< -C $(BUILD)
> $(MAKE) -C $(BUSYBOX_DIR) defconfig
> sed -i -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
>        -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' $(BUSYBOX_DIR)/.config
> yes "" | $(MAKE) -C $(BUSYBOX_DIR) oldconfig >/dev/null
> $(MAKE) -C $(BUSYBOX_DIR) -j$(JOBS)

# --- Our init --------------------------------------------------------------

$(INIT_BIN): init/init.c
> mkdir -p $(BUILD)
> gcc -static -Os -Wall -Wextra -o $@ $<

# --- Root filesystem, packed as an initramfs -------------------------------

$(INITRAMFS): $(BUSYBOX_BIN) $(INIT_BIN) $(shell find rootfs -type f)
> rm -rf $(ROOTFS)
> $(MAKE) -C $(BUSYBOX_DIR) CONFIG_PREFIX=$(ROOTFS) install >/dev/null
> mkdir -p $(ROOTFS)/{dev,proc,sys,tmp,root}
> cp -r rootfs/. $(ROOTFS)/
> cp $(INIT_BIN) $(ROOTFS)/init
> cd $(ROOTFS) && find . | cpio -o -H newc --owner=root:root --quiet | gzip -9 > $@
> @echo "Built $(INITRAMFS) ($$(du -h $@ | cut -f1))"

# --- Desktop system (Buildroot) --------------------------------------------
#
# Buildroot builds a whole Linux system from source: kernel, glibc, Xorg,
# Openbox and everything they need. Our part lives in desktop/ (the config
# and the files for the root filesystem) and init/ (built as package my-init).
# The first build downloads ~1 GB of sources and takes about an hour.

$(BR_TAR):
> mkdir -p $(BUILD)
> wget -q --show-progress -O $@.part https://buildroot.org/downloads/buildroot-$(BUILDROOT_VERSION).tar.xz
> mv $@.part $@

$(BR_DIR)/Makefile: $(BR_TAR)
> tar -xf $< -C $(BUILD)
> touch $@

# Buildroot can't handle spaces in the source path, so it works on a copy.
desktop: $(BR_DIR)/Makefile
> rm -rf $(BR_SRC)
> mkdir -p $(BR_SRC)
> cp -r desktop init panel login history search settings po $(BR_SRC)/
> chmod -R u=rwX,go=rX $(BR_SRC)
> # Programs and scripts: everything in these folders is one
> chmod 755 $(BR_SRC)/desktop/overlay/etc/init.d/* $(BR_SRC)/desktop/overlay/root/.xinitrc \
>   $(BR_SRC)/desktop/overlay/usr/bin/* $(BR_SRC)/desktop/overlay/usr/sbin/* \
>   $(BR_SRC)/desktop/overlay/usr/lib/my-distro/* $(BR_SRC)/desktop/post-build.sh \
>   $(BR_SRC)/desktop/post-fakeroot.sh \
>   $(BR_SRC)/desktop/overlay/usr/share/udhcpc/default.script.d/*
> $(BR_MAKE) qemu_x86_64_defconfig
> cd $(BR_OUT) && $(BR_DIR)/support/kconfig/merge_config.sh -m .config $(BR_SRC)/desktop/buildroot.fragment
> $(BR_MAKE) olddefconfig
> # The Software app's programs (make packages) are built in this tree too:
> # take them out of the root filesystem, they are not part of the image.
> $(PKG_CLEAN)
> $(BR_MAKE) my-init-dirclean my-panel-dirclean my-login-dirclean my-history-dirclean my-search-dirclean my-settings-dirclean my-ide-dirclean my-locale-dirclean
> # Buildroot doesn't notice changes to the kernel fragment by itself.
> if ls -d $(BR_OUT)/build/linux-[0-9]* >/dev/null 2>&1 && \
>    ! cmp -s desktop/linux-gui.fragment $(BR_OUT)/.linux-gui.fragment; then \
>   $(BR_MAKE) linux-reconfigure; fi
> # Xorg's screen saver extension (the screen lock asks it for the idle
> # time) is only built when libXScrnSaver was selected before Xorg was
> # built: a build folder from before 0.9.3 has an Xorg without it. Build it
> # again from scratch (only reconfiguring mixes old and new object files).
> X=$$(ls -d $(BR_OUT)/build/xserver_xorg-server-* 2>/dev/null | head -1); \
>   if [ -n "$$X" ] && [ -f "$$X/config.log" ] && ! grep -q -- '--enable-screensaver' "$$X/config.log"; then \
>   $(BR_MAKE) xserver_xorg-server-dirclean; fi
> # The same for what 0.9.5 added: Xorg draws with the graphics chip (glamor)
> # only when Mesa's EGL was there before it was built, GTK prints to CUPS
> # only when CUPS was. Build them again if theirs were built without.
> X=$$(ls -d $(BR_OUT)/build/xserver_xorg-server-* 2>/dev/null | head -1); \
>   if [ -n "$$X" ] && [ -f "$$X/config.log" ] && ! grep -q -- '--enable-glamor' "$$X/config.log"; then \
>   $(BR_MAKE) xserver_xorg-server-dirclean; fi
> # Xorg's drivers built with "bind now" don't load (see external.mk):
> # build them again with lazy binding.
> for d in modesetting:xserver_xorg-server fbdev:xdriver_xf86-video-fbdev; do \
>   f=$(BR_OUT)/target/usr/lib/xorg/modules/drivers/$${d%%:*}_drv.so; \
>   if [ -f "$$f" ] && $(BR_OUT)/host/bin/x86_64-linux-readelf -d "$$f" | grep -q BIND_NOW; then \
>     $(BR_MAKE) $${d##*:}-dirclean; fi; \
> done
> G=$$(ls -d $(BR_OUT)/build/libgtk3-* 2>/dev/null | head -1); \
>   if [ -n "$$G" ] && [ -f "$$G/.stamp_configured" ] && ! grep -rqs 'print_backends=file,cups' "$$G/build/meson-logs/meson-log.txt"; then \
>   $(BR_MAKE) libgtk3-dirclean; fi
> # Nor changes to BusyBox's (it builds in a minute, so start it again)
> if ! cmp -s desktop/busybox.fragment $(BR_OUT)/.busybox.fragment; then \
>   $(BR_MAKE) busybox-dirclean; fi
> # Nor changes to a package's own options (EAP for wpa_supplicant in 0.9.4,
> # screen capture for FFmpeg; 0.9.5: Mesa's drivers, the parts of libdrm
> # they need, Bluetooth firmware, fsck.fat, JPEG for imlib2): build these
> # again when their BR2_PACKAGE_*_ lines differ from the last build (or
> # there is no record of it yet).
> # 0.9.7: FFmpeg and mpv also when AV1 (dav1d), VA-API or PipeWire come
> # or go: they look for them only while they are built. The build
> # machine's Python when its modules change (fwupd-efi needs its lzma).
> for p in wpa_supplicant:WPA_SUPPLICANT_ 'ffmpeg:(FFMPEG_|DAV1D=|LIBVA=)' libdrm:LIBDRM_ \
>          mesa3d:MESA3D_ linux-firmware:LINUX_FIRMWARE_ dosfstools:DOSFSTOOLS_ \
>          imlib2:IMLIB2_ 'mpv:(MPV_|LIBVA=|PIPEWIRE=)' host-python3:HOST_PYTHON3_; do \
>   n=$${p%%:*}; v=$${p#*:}; \
>   ls -d $(BR_OUT)/build/$$n-[0-9]* >/dev/null 2>&1 || continue; \
>   if [ ! -f $(BR_OUT)/.config.last ] || \
>      ! cmp -s <(grep -E "^BR2_PACKAGE_$${v}" $(BR_OUT)/.config) \
>               <(grep -E "^BR2_PACKAGE_$${v}" $(BR_OUT)/.config.last); then \
>     $(BR_MAKE) $$n-dirclean; fi; \
> done
> $(BR_MAKE)
> cp desktop/linux-gui.fragment $(BR_OUT)/.linux-gui.fragment
> cp desktop/busybox.fragment $(BR_OUT)/.busybox.fragment
> cp $(BR_OUT)/.config $(BR_OUT)/.config.last
> mkdir -p out
> # Buildroot's ISO only boots from a CD. Make it again from the same files
> # (what grub-mkrescue does), so it also boots when it is written to a USB
> # stick: GRUB's boot sector for BIOS PCs (boot_hybrid.img, which starts the
> # CD boot image), and the small FAT image with GRUB for UEFI as a real
> # partition, which UEFI firmware looks for on USB sticks.
> [ -d $(ISO_FILES) ] || { echo "$(ISO_FILES) is missing"; exit 1; }
> # Buildroot's BIOS GRUB looks for its files on "(cd)" only. Build it again
> # with the prefix /boot/grub (no drive), so it uses the drive it started
> # from: the CD, or the USB stick. desktop/grub-bios-embedded.cfg searches too.
> G=$$(ls -d $(BR_OUT)/build/grub2-*/build-i386-pc/grub-core | head -1) && \
>   $(BR_OUT)/host/bin/grub-mkimage -d $$G -O i386-pc -p /boot/grub \
>     -c desktop/grub-bios-embedded.cfg -o $(BR_OUT)/build/my-distro-core.img \
>     boot linux normal iso9660 biosdisk part_msdos part_gpt all_video search search_fs_file && \
>   cat $$G/cdboot.img $(BR_OUT)/build/my-distro-core.img > $(ISO_FILES)/boot/grub/grub-eltorito.img
> # UEFI (0.9.7): the boot image the firmware reads (a small FAT image in
> # the ISO, also a partition on a USB stick) gets shim as BOOTX64.EFI,
> # which starts GRUB (grubx64.efi, signed for Secure Boot by make
> # desktop-secureboot), MokManager and the certificate to enroll
> # (all from /usr/lib/my-distro/efi, see desktop/package/my-secureboot).
> E=$(BR_OUT)/target/usr/lib/my-distro/efi; F=$(ISO_FILES)/boot/fat.efi; \
>   M=$$(ls $(BR_OUT)/host/sbin/mkfs.fat $(BR_OUT)/host/bin/mkfs.fat 2>/dev/null | head -1); \
>   [ -f $$E/grubx64.efi ] && [ -n "$$M" ] || { echo "UEFI boot files or mkfs.fat missing"; exit 1; }; \
>   rm -f $$F && $$M -C -n MY_DISTRO $$F 8192 >/dev/null && \
>   $(BR_OUT)/host/bin/mmd -i $$F ::/EFI ::/EFI/BOOT && \
>   $(BR_OUT)/host/bin/mcopy -i $$F $$E/BOOTX64.EFI $$E/grubx64.efi $$E/mmx64.efi \
>     $$E/my-distro-secureboot.cer ::/EFI/BOOT/
> rm -f out/my-distro.iso
> $(BR_OUT)/host/bin/xorriso -as mkisofs -o out/my-distro.iso -J -R -V MY_DISTRO \
>   -b boot/grub/grub-eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
>   --grub2-boot-info --grub2-mbr $(BR_OUT)/target/lib/grub/i386-pc/boot_hybrid.img \
>   -eltorito-alt-boot --efi-boot boot/fat.efi -efi-boot-part --efi-boot-image \
>   --protective-msdos-label $(ISO_FILES) 2>&1 | grep -v "^xorriso : UPDATE" || true
> [ -s out/my-distro.iso ]
> cp $(BR_OUT)/images/rootfs.tar.xz $(UPDATE)
> @echo "ISO: out/my-distro.iso ($$(du -h out/my-distro.iso | cut -f1))"
> @echo "Update package: $(UPDATE) ($$(du -h $(UPDATE) | cut -f1)); sign it with: make release-files"

# The files of a release besides the ISO: the update package that installed
# systems download (System update), its SHA-256 sum, and a signature of that
# sum. The signing key stays on this machine, outside the repository; its
# public half is desktop/overlay/etc/my-distro/update-key.pem. Make a key
# once with:
#   openssl genpkey -algorithm ed25519 -out ~/.config/my-distro/update-signing-key.pem
#   openssl pkey -in ~/.config/my-distro/update-signing-key.pem -pubout \
#     -out desktop/overlay/etc/my-distro/update-key.pem
release-files:
> [ -f $(UPDATE) ] || { echo "No $(UPDATE): run make desktop first"; exit 1; }
> cd out && sha256sum my-distro-update.tar.xz > my-distro-update.tar.xz.sha256
> openssl pkeyutl -sign -rawin -inkey $(SIGNING_KEY) -in $(UPDATE).sha256 -out $(UPDATE).sha256.sig
> openssl pkeyutl -verify -pubin -inkey desktop/overlay/etc/my-distro/update-key.pem \
>   -rawin -in $(UPDATE).sha256 -sigfile $(UPDATE).sha256.sig
> @echo "Upload to the release: out/my-distro.iso $(UPDATE) $(UPDATE).sha256 $(UPDATE).sha256.sig"

# --- Secure Boot --------------------------------------------------------------
#
# my-distro starts with Secure Boot on through Debian's shim (signed by
# Microsoft), which starts GRUB if it is signed with a key the PC's owner
# enrolled once (MokManager). secureboot-key makes that key: an RSA key and
# its certificate. The key stays in SB_KEY_DIR (outside the repository) and
# is used only by desktop-secureboot; the certificate goes into the
# repository and onto the ISO (desktop/package/my-secureboot).
secureboot-key:
> [ ! -e $(SB_KEY) ] || { echo "$(SB_KEY) exists already; keep using it"; exit 1; }
> mkdir -p -m 700 $(SB_KEY_DIR)
> umask 077 && openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
>   -subj "/CN=my-distro Secure Boot 2026/" \
>   -addext "basicConstraints=critical,CA:FALSE" -addext "keyUsage=digitalSignature" \
>   -addext "extendedKeyUsage=codeSigning" \
>   -keyout $(SB_KEY) -out $(SB_KEY_DIR)/my-distro-secureboot.crt 2>/dev/null
> mkdir -p $(dir $(SB_CERT))
> cp $(SB_KEY_DIR)/my-distro-secureboot.crt $(SB_CERT)
> if [ -n "$(SB_BACKUP)" ]; then mkdir -p "$(SB_BACKUP)" && \
>   cp $(SB_KEY) $(SB_KEY_DIR)/my-distro-secureboot.crt "$(SB_BACKUP)/"; fi
> @echo "Key: $(SB_KEY) (keep it private, and a copy somewhere safe). Certificate: $(SB_CERT)"

# make desktop, with GRUB, the kernel and fwupd's UEFI program signed with
# the Secure Boot key (desktop/post-build.sh signs them before the images
# are made)
desktop-secureboot:
> [ -f $(SB_KEY) ] || { echo "No Secure Boot key $(SB_KEY): make secureboot-key"; exit 1; }
> openssl x509 -in $(SB_CERT) -noout -pubkey | cmp -s - <(openssl pkey -in $(SB_KEY) -pubout) || \
>   { echo "$(SB_KEY) does not belong to $(SB_CERT)"; exit 1; }
> # (BUILD again: this Makefile keeps it from sub-makes, see MAKEOVERRIDES)
> $(MAKE) desktop BUILD=$(BUILD) MY_SB_KEY=$(SB_KEY)

# --- Software app packages (my-pkg) -------------------------------------------
#
# Extra programs users install without a new image: desktop/packages.fragment
# says which, desktop/packages.catalog how they look in the Software app.
# They are built on top of the image's configuration in the same tree (so
# they link against exactly the image's libraries); desktop/make-packages.py
# then packs each package's files that the image doesn't have into its own
# .tar.xz and writes the package list. Afterwards the tree is the image's
# again. Needs a finished `make desktop` (its update package tells which
# files the image has). The first run builds for about an hour.
packages: $(BR_DIR)/Makefile
> [ -f $(UPDATE) ] && [ -f $(BR_OUT)/.config.last ] || { echo "Build the image first: make desktop"; exit 1; }
> rm -rf $(PKG_WORK)
> mkdir -p $(PKG_WORK)
> # What the image has, then the image plus the packages
> cp $(BR_OUT)/.config.last $(BR_OUT)/.config
> $(BR_MAKE) -s --no-print-directory show-info > $(PKG_WORK)/base-info.json
> cp desktop/packages.fragment $(PKG_WORK)/packages.fragment
> cd $(BR_OUT) && $(BR_DIR)/support/kconfig/merge_config.sh -m .config $(PKG_WORK)/packages.fragment
> $(BR_MAKE) olddefconfig
> $(BR_MAKE) -s --no-print-directory show-info > $(PKG_WORK)/info.json
> # Stops if the packages would change the image's own packages; build tools
> # whose options changed are built again.
> r=$$($(PKG_TOOL) check-config $(BR_OUT) $(PKG_WORK) desktop/packages.fragment) || exit 1; \
>   for p in $$r; do $(BR_MAKE) $$p-dirclean || exit 1; done
> # Build everything and finalize the target folder as for the image (strip
> # programs, remove headers and manuals), but make no image
> $(BR_MAKE) $(if $(PKG_JOBS),BR2_JLEVEL=$(PKG_JOBS)) target-finalize
> $(PKG_TOOL) cut $(BR_OUT) $(PKG_WORK) $(BR_DIR) $(UPDATE) desktop/packages.catalog $(PKG_OUT)
> cp $(BR_OUT)/.config $(BR_OUT)/.config.packages-last
> # Back to the image's configuration and files
> cp $(BR_OUT)/.config.last $(BR_OUT)/.config
> $(PKG_CLEAN)
> @echo "Packages: $(PKG_OUT) ($$(du -sh $(PKG_OUT) | cut -f1)); sign the list with: make release-packages"

# Signs the package list with the same key and in the same way as
# release-files signs the update (Ed25519 over the whole file). The list has
# the SHA-256 sum of every package, so this one signature covers them all.
release-packages:
> [ -f $(PKG_INDEX) ] || { echo "No $(PKG_INDEX): run make packages first"; exit 1; }
> openssl pkeyutl -sign -rawin -inkey $(SIGNING_KEY) -in $(PKG_INDEX) -out $(PKG_INDEX).sig
> openssl pkeyutl -verify -pubin -inkey desktop/overlay/etc/my-distro/update-key.pem \
>   -rawin -in $(PKG_INDEX) -sigfile $(PKG_INDEX).sig
> @echo "Upload everything in $(PKG_OUT)/ to the same release as the update package."

# Boot the ISO like a VM would (BIOS, 2 GB RAM).
run-iso:
> qemu-system-x86_64 $(KVM) -m 2048 -smp 2 -cdrom out/my-distro.iso \
>   -vga std -device qemu-xhci -device usb-tablet -nic user,model=e1000

# Test the installer: boot the ISO with an empty 8 GB disk (virtio), run
# "Install to disk", power off, then boot the installed system with run-disk.
DISK_IMG := $(BUILD)/disk.img

run-install:
> [ -f $(DISK_IMG) ] || truncate -s 8G $(DISK_IMG)
> qemu-system-x86_64 $(KVM) -m 2048 -smp 2 -cdrom out/my-distro.iso \
>   -drive file=$(DISK_IMG),if=virtio,format=raw -boot d \
>   -vga std -device qemu-xhci -device usb-tablet -nic user,model=e1000

run-disk:
> qemu-system-x86_64 $(KVM) -m 2048 -smp 2 \
>   -drive file=$(DISK_IMG),if=virtio,format=raw \
>   -vga std -device qemu-xhci -device usb-tablet -nic user,model=e1000

run-desktop:
> qemu-system-x86_64 $(KVM) -M pc -m 1024 -smp 2 \
>   -kernel $(BR_OUT)/images/bzImage \
>   -drive file=$(BR_OUT)/images/rootfs.ext2,if=virtio,format=raw \
>   -append "root=/dev/vda rw console=tty1 console=ttyS0 init=/sbin/my-init" \
>   -vga std -device qemu-xhci -device usb-tablet \
>   -nic user,model=virtio-net-pci \
>   -serial mon:stdio

clean:
> rm -rf $(INIT_BIN) $(ROOTFS) $(INITRAMFS)

# Deletes all downloads and builds, including the kernel.
distclean:
> rm -rf $(BUILD)
