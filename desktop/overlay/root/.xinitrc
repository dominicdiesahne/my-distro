#!/bin/sh
# The desktop session. my-login runs this as the user who logged in;
# when it ends (Log out = Openbox exits), the login screen comes back.

cd "$HOME"  # programs started from here open in the home folder
xrdb -nocpp -merge "$HOME/.Xresources"

# GTK apps (the file manager) look for an accessibility service we don't have.
export NO_AT_BRIDGE=1

# Openbox starts first, then runs the autostart script: taskbar, desktop
# icons, a terminal. Starting them after Openbox means it already manages
# the screen when they appear.
exec openbox --startup "sh $HOME/.config/openbox/autostart"
