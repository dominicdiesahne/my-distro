#!/bin/sh
# Runs while Buildroot makes each image (ISO, update package), after it has
# added the users (users.table) and set the files' owners and permissions,
# just before packing. $1 = the folder with the finished root filesystem.
set -e
T="$1"

# Factory reset (my-reset): the first state of everything that updates keep
# (users and passwords, home folders, settings in /etc, /var, ...), copied
# from this fresh image with owners and permissions. A reset puts these
# copies back; every update brings the new version's. Only root may look
# inside (the password files are in it).
F="$T/usr/share/my-distro/factory"
rm -rf "$F"
mkdir -p "$F"
chmod 700 "$F"
python3 "$T/usr/bin/my-update" keep-list | while read -r k; do
	case "$k" in
	tmp|run|proc|sys|dev|mnt|media|lost+found|boot/*|.my-update|etc/fstab) continue ;;
	esac
	[ -e "$T/$k" ] || [ -L "$T/$k" ] || continue
	mkdir -p "$F/$(dirname "$k")"
	cp -a "$T/$k" "$F/$k"
done
grep -q '^root:' "$F/etc/shadow" || { echo "post-fakeroot: factory copy failed"; exit 1; }
