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

# The live system starts without /etc/my-distro/setup-done, so the setup runs.
rm -f "$T/etc/my-distro/setup-done"
