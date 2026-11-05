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

# The normal user (users.table) gets the same desktop setup as root: copy
# root's home over. Buildroot makes the files theirs when it builds the image.
mkdir -p "$T/home/dominic"
cp -a "$T/root/." "$T/home/dominic/"
rm -f "$T/home/dominic/.ash_history"
