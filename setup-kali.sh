#!/bin/sh
# One-time setup of the Kali (WSL) build machine: brings Kali up to date and
# installs the build tools. Asks for your sudo password.
set -e

# Kali replaced its package signing key in April 2025. Older installs can't
# verify the package lists without the new key, so `apt update` fails.
echo "Installing Kali's current signing key..."
sudo wget -q https://archive.kali.org/archive-keyring.gpg -O /usr/share/keyrings/kali-archive-keyring.gpg

# WSL mounts /tmp/.X11-unix read-only (it shows Linux windows on Windows).
# systemd's upgrade script tries to change it and fails, so unmount it during
# the upgrade. `wsl --shutdown` in Windows brings it back.
if findmnt -n -o OPTIONS /tmp/.X11-unix 2>/dev/null | grep -q '^ro'; then
	echo "Unmounting the read-only /tmp/.X11-unix for the upgrade..."
	sudo umount /tmp/.X11-unix
fi

# Keep your existing config files when a package ships a new version of one.
APT="sudo DEBIAN_FRONTEND=noninteractive apt-get -y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold"

echo "Finishing any interrupted upgrade..."
sudo dpkg --configure -a

echo "Updating package lists..."
$APT update

echo "Upgrading Kali (Kali is a rolling release and expects full upgrades)..."
$APT full-upgrade

echo "Installing build tools and QEMU..."
$APT install bison flex libelf-dev libncurses-dev qemu-system-x86 qemu-system-gui

echo "Done. Run 'wsl --shutdown' in Windows later to restore Linux GUI windows."
