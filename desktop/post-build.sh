#!/bin/sh
# Runs after Buildroot has filled the root filesystem. $1 = the target folder.
set -e
T="$1"

# Xorg's own start script would launch a bare X server (no desktop) before
# S90desktop runs, and S90desktop's X server would then fail with
# "Server is already active for display 0".
rm -f "$T/etc/init.d/S40xorg"
# The audit package's own start script: ours (S03audit) runs auditd under
# my-keepalive and loads our rules.
rm -f "$T/etc/init.d/S02auditd"

# Crontabs: only root may look into the folder (crontab -e works through
# BusyBox's setuid crontab); root's own one has to be private too.
mkdir -p "$T/etc/cron/crontabs"
chmod 700 "$T/etc/cron/crontabs"

# Desktop icons are launchers; the file manager wants them executable.
chmod 755 "$T"/root/Desktop/*.desktop

# Templates for "Create New..." and the sample documents. Copied from Windows
# they are all executable, which makes the file manager treat them as programs.
# (It copies a template's content but not its permissions, so making the
# script templates executable would not help either: run them with
# python3 file.py / sh file.sh.)
chmod 644 "$T"/root/Templates/* "$T"/root/Documents/*

# Openbox settings for root: the stock rc.xml plus our keyboard shortcuts
# and a dark window theme.
mkdir -p "$T/root/.config/openbox"
sed -e 's|<name>Clearlooks</name>|<name>my-distro</name>|' \
    -e 's|<number>4</number>|<number>1</number>|' \
    -e 's|kfmclient openProfile filemanagement|pcmanfm ~|' \
    -e 's|<name>Konqueror</name>|<name>Files</name>|' \
    -e 's|<chainQuitKey>C-g</chainQuitKey>|&\
  <keybind key="W-space"><action name="Execute"><command>killall -USR1 my-panel</command></action></keybind>\
  <keybind key="A-F1"><action name="Execute"><command>killall -USR1 my-panel</command></action></keybind>\
  <keybind key="W-s"><action name="Execute"><command>my-search-gui</command></action></keybind>\
  <keybind key="W-t"><action name="Execute"><command>xterm</command></action></keybind>\
  <keybind key="C-A-t"><action name="Execute"><command>xterm</command></action></keybind>\
  <keybind key="W-Up"><action name="ToggleMaximize"/></keybind>\
  <keybind key="W-Down"><action name="Iconify"/></keybind>|' \
    "$T/etc/xdg/openbox/rc.xml" > "$T/root/.config/openbox/rc.xml"
# Shortcuts added since 0.9.2 (screenshots, volume, screen lock, brightness):
# from files that installed-fixups also adds to the rc.xml of users from
# older versions.
for keys in "$T"/usr/share/my-distro/openbox-keys-*.xml; do
	sed -i "/<chainQuitKey>/r $keys" "$T/root/.config/openbox/rc.xml"
done

# sudo reads /etc/sudoers.d (our rule for the group wheel) only when the
# files there belong to root and can't be written by others
chmod 750 "$T/etc/sudoers.d"
chmod 440 "$T/etc/sudoers.d/wheel"
grep -q '^@includedir /etc/sudoers.d' "$T/etc/sudoers" ||
	echo '@includedir /etc/sudoers.d' >>"$T/etc/sudoers"

# Every new user's home folder starts as a copy of /etc/skel (my-setup makes
# it): the same desktop setup as root's.
mkdir -p "$T/etc/skel"
cp -a "$T/root/." "$T/etc/skel/"
rm -f "$T/etc/skel/.ash_history"

# The demo user of the live system (users.table) gets it too. Buildroot makes
# the files theirs when it builds the image. The first-boot setup replaces
# this account with your own.
mkdir -p "$T/home/dominic"
cp -a "$T/etc/skel/." "$T/home/dominic/"

# The version is written in one place, /etc/my-distro/version (my-update
# compares it with the newest release); the welcome message shows it.
sed -i "s/@VERSION@/$(cat "$T/etc/my-distro/version")/" "$T/etc/motd"

# Firmware (graphics, Wi-Fi, network): xz-compress it. The kernel loads
# NAME.xz when NAME is missing (CONFIG_FW_LOADER_COMPRESS_XZ); its xz decoder
# needs the CRC32 check. regulatory.db stays as it is (tiny, and signed).
# Links (linux-firmware has many, e.g. iwlwifi-*.ucode -> intel/iwlwifi/...)
# then point to the .xz files.
if [ -d "$T/lib/firmware" ]; then
	(
	cd "$T/lib/firmware"
	find . -type f ! -name '*.xz' ! -name 'regulatory.db*' -exec xz -C crc32 -T0 -f {} +
	find . -type l ! -name '*.xz' | while read -r link; do
		target=$(readlink "$link")
		if [ -e "$(dirname "$link")/$target.xz" ]; then
			ln -sfn "$target.xz" "$link.xz"
			rm -f "$link"
		fi
	done
	)
fi

# Saved Wi-Fi passwords: only root may read them
chmod 600 "$T/etc/wpa_supplicant.conf"

# Printing (0.9.5). CUPS runs a backend as root only when nobody else may
# read it: the "Save as PDF" printer's backend writes into home folders.
chmod 700 "$T/usr/lib/cups/backend/my-pdf"
# cups-browsed (cups-filters) would make queues for every printer in the
# network by itself and listen for printers announcing themselves; CUPS and
# the Printers window find them without it.
rm -f "$T/etc/init.d/S82cups-browsed"

# The live system starts without /etc/my-distro/setup-done, so the setup runs.
rm -f "$T/etc/my-distro/setup-done"
# (The copy for the factory reset is made later, in post-fakeroot.sh.)

# --- 0.9.7 -------------------------------------------------------------------

# Bluetooth sound moved from bluez-alsa to PipeWire. Buildroot doesn't take
# a package's files out of the root filesystem when it is no longer chosen,
# so a build folder from before 0.9.7 still has bluez-alsa's daemon and ALSA
# plugin (both would fight PipeWire over the headphones).
rm -f "$T/usr/bin/bluealsad" "$T/usr/bin/bluealsa" "$T/usr/bin/bluealsa-aplay" \
      "$T/usr/bin/bluealsactl" "$T/usr/bin/bluealsa-cli" "$T/usr/bin/bluealsa-rfcomm" \
      "$T/etc/init.d/S41bluealsa" "$T/etc/dbus-1/system.d/bluealsa.conf" \
      "$T/etc/dbus-1/system.d/org.bluealsa.conf" \
      "$T"/usr/share/alsa/alsa.conf.d/20-bluealsa.conf \
      "$T"/usr/lib/alsa-lib/libasound_module_*bluealsa*.so

# PipeWire is started in each session (/usr/lib/my-distro/autostart), not
# as a system service, and finds its devices with udev itself.
rm -f "$T/etc/init.d/S50pipewire" "$T/etc/init.d/S51wireplumber"

# Samba (a Software app package): its own start script would start the
# server at every boot; S91sharing starts it only when Sharing is on.
rm -f "$T/etc/init.d/S91smb"

# UEFI GRUB for shim (Secure Boot): one file with every module it needs
# built in (with Secure Boot on, GRUB loads no modules from the disk), and
# the SBAT data shim checks (desktop/secureboot/grub-sbat.csv). The ISO's
# boot image (Makefile) and my-install take it from /usr/lib/my-distro/efi.
D=$(dirname "$0")
G=$(ls -d "$BUILD_DIR"/grub2-[0-9]*/build-x86_64-efi/grub-core 2>/dev/null | head -n 1)
if [ -n "$G" ] && [ -d "$T/usr/lib/my-distro/efi" ]; then
	"$HOST_DIR/bin/grub-mkimage" -d "$G" -O x86_64-efi -p /boot/grub \
		-c "$D/grub-efi-embedded.cfg" --sbat "$D/secureboot/grub-sbat.csv" \
		-o "$T/usr/lib/my-distro/efi/grubx64.efi" \
		boot linux normal configfile iso9660 fat ext2 part_msdos part_gpt \
		all_video efi_gop efi_uga video_bochs video_cirrus gfxterm \
		search search_fs_file search_fs_uuid search_label \
		echo test true sleep reboot halt ls cat loadenv minicmd gzio regexp probe
fi

# make desktop-secureboot: sign GRUB, the kernel (the one in the image for
# my-install and the one GRUB starts from the ISO) and fwupd's UEFI program
# with my-distro's Secure Boot key (MY_SB_KEY; the key stays outside the
# image and the repository). Files signed with it already are left alone.
CERT="$D/secureboot/my-distro-secureboot.crt"
sb_sign() {
	[ -f "$1" ] || return 0
	"$HOST_DIR/bin/osslsigncode" verify -in "$1" -CAfile "$CERT" >/dev/null 2>&1 && return 0
	"$HOST_DIR/bin/osslsigncode" sign -h sha256 -certs "$CERT" -key "$MY_SB_KEY" \
		-in "$1" -out "$1.signed" >/dev/null &&
		mv "$1.signed" "$1" &&
		"$HOST_DIR/bin/osslsigncode" verify -in "$1" -CAfile "$CERT" >/dev/null ||
		{ echo "post-build: signing $1 failed"; exit 1; }
	echo "post-build: signed $1 for Secure Boot"
}
if [ -n "$MY_SB_KEY" ]; then
	sb_sign "$T/usr/lib/my-distro/efi/grubx64.efi"
	sb_sign "$T/boot/bzImage"
	sb_sign "$BINARIES_DIR/bzImage"
	# fwupd starts its UEFI program through shim and takes the .signed one
	F="$T/usr/libexec/fwupd/efi/fwupdx64.efi"
	if [ -f "$F" ]; then
		cp "$F" "$F.signed"
		sb_sign "$F.signed"
	fi
fi
