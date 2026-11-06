#!/bin/sh
# Runs after Buildroot has filled the root filesystem. $1 = the target folder.
set -e
T="$1"

# Xorg's own start script would launch a bare X server (no desktop) before
# S90desktop runs, and S90desktop's X server would then fail with
# "Server is already active for display 0".
rm -f "$T/etc/init.d/S40xorg"

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
# Shortcuts added in 0.9.2 (screenshots, volume keys): from a file that
# installed-fixups also adds to the rc.xml of users from older versions.
sed -i "/<chainQuitKey>/r $T/usr/share/my-distro/openbox-keys-0.9.2.xml" \
    "$T/root/.config/openbox/rc.xml"

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

# The live system starts without /etc/my-distro/setup-done, so the setup runs.
rm -f "$T/etc/my-distro/setup-done"
