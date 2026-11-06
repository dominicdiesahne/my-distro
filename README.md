# my-distro

A small Linux distribution built from source, with its own init system,
desktop pieces and two features most desktops don't have: **system-wide undo**
for every file change and **AI semantic file search** that finds files by
meaning. It boots as a live ISO in a VM and can install itself onto a disk.

Most of the parts that make it *this* distro are written in plain C and kept
short and commented, so you can read how an OS fits together: `init` (PID 1),
the taskbar, the login screen, the file history daemon and the search indexer.

![Desktop with taskbar, Start menu and app icons](docs/screenshots/desktop.png)

| Login screen | Semantic search |
|---|---|
| ![Login screen](docs/screenshots/login.png) | ![Search files](docs/screenshots/search.png) |
| **File history (undo)** | **Web browser** |
| ![File history](docs/screenshots/history.png) | ![NetSurf browser](docs/screenshots/browser.png) |
| **Settings** | **Clock and calendar** |
| ![Settings app](docs/screenshots/settings.png) | ![Clock app](docs/screenshots/clock.png) |
| **Another theme, taskbar on top, analog clock** | **Calendar (click the clock)** |
| ![Violet theme](docs/screenshots/themes.png) | ![Calendar](docs/screenshots/calendar.png) |
| **First-boot setup** | **Users and system** |
| ![Setup](docs/screenshots/setup.png) | ![Users and system](docs/screenshots/users.png) |

![LazyVim IDE with file explorer and syntax highlighting](docs/screenshots/ide.png)

## Features

- **Own init in C** (`init/init.c`) as PID 1: mounts filesystems, runs the boot
  scripts, starts a login on the console, reaps zombies, reboots/powers off.
- **First-boot setup** (`settings/my-setup.c`): before the first login it
  asks for the keyboard layout, your name, user name and password, the
  computer's name and a color theme, and creates your account.
- **Users and system** (same program, Start menu or Settings): add and
  remove users, change passwords, the computer name and the keyboard layout.
  It asks for the root password; `login/my-admin.c` checks it.
- **System updates** (`my-update`): every new release on GitHub shows up as
  an *Update to …* button in the taskbar of installed systems. One click (and
  the root password) downloads it, checks its signature and installs it;
  your files, users and settings stay. See [System updates](#system-updates).
- **Graphical login** (`login/my-login.c`): checks passwords against
  `/etc/shadow`, drops root rights and starts your desktop as you.
- **Taskbar** (`panel/my-panel.c`): Start menu with icons, a button per window
  with the app's icon, and a clock (digital or a small analog face) that opens
  a calendar when you click it.
- **Settings app** (`settings/my-settings.c`): six color themes or your own
  accent color, wallpaper, clock style and format, taskbar position and size.
  Changes apply at once to the taskbar, window borders, wallpaper, terminal
  and the login screen.
- **Clock app** (`settings/my-clock.c`): a big analog or digital clock.
- **IDE:** Neovim with [LazyVim](https://www.lazyvim.org/) (*Code editor* in
  the Start menu, or `my-ide [file]`). Plugins and syntax parsers for 32
  languages come preinstalled, so it works without internet. git, ripgrep
  and fd included.
- **System-wide undo** (`history/`): every file that any program changes is
  saved as a version (kernel *fanotify*). Undo from a window or with `my-undo`.
- **Semantic file search** (`search/`): an embedding model (all-MiniLM-L6-v2 via
  llama.cpp) indexes your files by meaning. *"customer agreement for training
  subscription"* finds a note about a client signing up for a gym membership,
  without a single shared word.
- **Desktop:** Xorg, Openbox, PCManFM file manager with desktop icons, NetSurf
  web browser (HTTPS), xterm, nano, htop, calculator, generated wallpapers.
  Right-click in a Files window, *Create New…* makes text, Markdown, shell, Python, C, HTML,
  CSS, JavaScript, JSON, CSV, Lua, Makefile and config files from templates
  (`~/Templates`).
- **Python 3** with pip, so `.py` scripts run (`python3 file.py`).
- **Live ISO** for BIOS and UEFI, from a CD or a USB stick; tested in
  VirtualBox and QEMU. Drivers for real PCs: Intel and AMD graphics,
  Realtek and Intel network cards, Wi-Fi, laptop touchpads.
- **Wi-Fi** (`settings/my-wifi.c`): bars in the taskbar show the signal; a
  click lists the networks around, asks for the password and connects
  (wpa_supplicant underneath; started by a udev rule when an adapter appears).
- **Installer** (`my-install`): copies the live system onto a hard disk with
  GRUB for BIOS and UEFI, so your files and settings survive a reboot.
- **Sound**: a speaker in the taskbar shows the volume. Scroll on it for
  louder/quieter, click to mute, right-click for all controls (alsamixer).
  The volume keys work too (`my-volume up|down|mute`). Your volume is
  remembered for the next login. Drivers for the sound chips of most PCs
  (Intel HD Audio with Realtek, Conexant, Cirrus, IDT, VIA codecs, HDMI)
  and USB headsets.
- **Notifications** (`settings/my-notifyd.c`): small boxes in the corner
  next to the taskbar, in the colors of your theme. Any program can send one
  over the standard freedesktop D-Bus interface; from a script:
  `my-notify "Title" "Text"` (also called `notify-send`).
- **USB sticks and SD cards** open by themselves in `/media/NAME`, with a
  notification: FAT, exFAT, NTFS (Windows), ext4, CDs/DVDs. To remove one
  safely: right-click its folder, *Eject* (or Start menu, *Eject USB stick*,
  or `my-eject`). Internal disks are never opened by themselves.
- **Screenshots** (`panel/my-screenshot.c`): Print = whole screen,
  Alt+Print = the window in front, Shift+Print = drag an area; or Start
  menu, *Screenshot*. They go to `~/Pictures`.
- **Firewall** (nftables): nobody can connect to this computer from outside;
  everything you start (web, updates) works as before. Rules in
  `/etc/nftables.conf`, your own in `/etc/nftables.d/`.
- **Time from the internet**: the clock sets itself (BusyBox `ntpd`,
  `pool.ntp.org`) and keeps the hardware clock right too.
- **sudo**: administrators run commands as root with their own password.
  The account from the setup is one (when it shares its password with root);
  *Users and system* makes others administrators.
- **Laptops**: the battery's charge in the taskbar (a warning below 10 %),
  *Suspend* in the Start menu.

## Try it

**Don't want to build it?** Download a ready ISO from
[Releases](https://github.com/dominicdiesahne/my-distro/releases/latest)
and go to step 2.

### 1. Build the ISO

You need a Linux machine (or WSL2 on Windows) with about **20 GB of free disk**
and a few hours of time for the first build. Buildroot compiles everything
from source: the toolchain is prebuilt, but the kernel, Xorg, GTK and the rest
are not. Rebuilds after that take minutes.

Build tools (Debian/Ubuntu/Kali):

```bash
sudo apt install build-essential bc bison flex libelf-dev libssl-dev libncurses-dev cpio wget rsync unzip file patch perl bzip2 python3 qemu-system-x86
```

Then:

```bash
git clone https://github.com/dominicdiesahne/my-distro.git
cd my-distro
make desktop
```

The result is **`out/my-distro.iso`** (~240 MB). Build files go to
`~/my-distro-build` (change with `make desktop BUILD=/some/path`), not into
the repository. Kali on WSL: `setup-kali.sh` fixes the usual Kali/WSL update
problems and installs the tools.

### 2. Boot it

```bash
make run-iso      # QEMU
```

**VirtualBox:** New VM, type *Linux*, version **Other Linux (64-bit)** (it must
be a 64-bit type, otherwise the kernel can't start and the screen stays black),
2048 MB RAM, 2 CPUs, no hard disk. Settings, Storage: put `my-distro.iso` in
the optical drive. Display: graphics controller *VMSVGA*. Start.

**VMware:** new VM from the ISO, *Other Linux 6.x kernel 64-bit*, 2 GB RAM.
**Hyper-V:** Generation 2 VM, 2048 MB RAM, **Secure Boot off**, ISO in the DVD drive.

In the boot menu, *safe graphics* helps if the screen stays black.

#### On a real PC

Write the ISO to a USB stick (at least 1 GB; **everything on it is
deleted**):

- **Windows:** [Rufus](https://rufus.ie): choose the ISO, and when it asks,
  **"Write in DD image mode"**. Or [balenaEtcher](https://etcher.balena.io).
- **Linux:** `sudo dd if=my-distro.iso of=/dev/sdX bs=4M status=progress && sync`
  (`/dev/sdX` = the stick, check with `lsblk`; the wrong letter overwrites
  another disk).

Then start the PC from the stick (the boot menu key is usually F12, F11, F8
or Esc). In the firmware settings, **Secure Boot must be off**: my-distro's
boot loader is not signed. The live system runs from RAM and changes nothing
on the PC's disk unless you run *Install to disk*.

What works: Intel and AMD graphics (the screen's resolution, several
monitors; no 3D acceleration), wired network with Intel or Realtek cards,
Wi-Fi (below), laptop touchpads (I2C) and USB keyboards and mice, NVMe,
SATA and USB disks, Intel HD Audio. NVIDIA graphics only get the basic
firmware screen.

**Wi-Fi:** click the Wi-Fi bars in the taskbar (or Start menu, *Wi-Fi*),
pick a network, type its password. my-distro remembers it
(`/etc/wpa_supplicant.conf`, kept by system updates) and connects again by
itself. Drivers and firmware are in for Intel, Qualcomm Atheros, MediaTek,
Realtek and Broadcom chips, and most USB Wi-Fi sticks. Networks without a
name (hidden) and enterprise Wi-Fi (eduroam, username + password) can't be
chosen in the window yet; `wpa_cli` in a terminal can.

If something doesn't work, a terminal shows what the PC has: `lspci -k`
(each device and the driver that took it), `dmesg | grep -i firmware`.

### 3. Set it up and log in

The **setup** comes first: keyboard layout, your account (name, user name,
password), a name for the computer and a color theme. Your password is also
the administrator (root) password, unless you untick that and choose another.
Then log in with your new account.

On the live system you can **skip** the setup and use the demo account:

| User | Password | |
|---|---|---|
| `dominic` | `dominic` | normal user, pre-filled on the login screen |
| `root` | `root` | administrator |

The live system runs in RAM and forgets everything (also the account from the
setup) when it shuts down, unless you [install it](#4-install-to-a-disk-optional).
Change users and passwords later in **Users and system** (Start menu), or your
own password with `passwd`.

### 4. Install to a disk (optional)

The live system forgets everything when it shuts down. To keep your files,
install it onto a hard disk:

1. Give the VM a hard disk: **VirtualBox** Settings, Storage, add a hard disk
   (8 GB is plenty, dynamically allocated). In QEMU: `make run-install`.
2. Boot the ISO and log in, then double-click **Install to disk** on the
   desktop (or Start menu, *Install to disk*; or `my-install` as root).
   It asks for the root password (yours from the setup, or `root` if you
   skipped it).
3. Pick the disk, type `yes`. **Everything on that disk is deleted.**
   It copies the system (a minute or two) and installs GRUB. If you did the
   setup, your account is copied too; if you skipped it, the installed system
   starts with the setup.
4. Power off, remove the ISO from the optical drive (VirtualBox: Storage,
   click the CD, *Remove disk from virtual drive*), start the VM again.
   QEMU: `make run-disk`.

It now boots from the disk, and changes stay. The disk gets three
partitions: 1 MB for GRUB on BIOS PCs, 256 MB EFI system partition for UEFI,
the rest is the system (ext4). It boots with BIOS and UEFI alike.

### Keyboard shortcuts

| Shortcut | Does |
|---|---|
| Super+Space or Alt+F1 | Start menu |
| Super+S | Search files |
| Super+T or Ctrl+Alt+T | Terminal |
| Super+E | File manager |
| Super+Up / Super+Down | Maximise / minimise |
| Print / Alt+Print / Shift+Print | Screenshot: screen / window / area |
| Volume keys, Sleep key | Louder, quieter, mute; suspend |
| Alt+Tab, Alt+F4 | Switch / close windows |
| Ctrl+Alt+F1 / Ctrl+Alt+F7 | Text console / back to the desktop |

The keyboard layout is German (`desktop/overlay/etc/X11/xorg.conf.d/00-keyboard.conf`).

## How it works

### Boot

1. GRUB loads the kernel and the whole root filesystem into RAM (initrd).
2. The kernel starts our init, `/sbin/my-init` (`init/init.c`).
3. init mounts `/proc`, `/sys`, `/dev`, `/tmp` and runs `/etc/init.d/rcS`:
   udev, `S35firewall`, network (DHCP), `S45sound`, `S48time` (clock from
   the internet), `S50history` (file history), `S90desktop`.
4. `S90desktop` starts the X server with **`my-login`** as its only client.
5. After login, `my-login` drops root rights and runs `~/.xinitrc` as you,
   which starts the session D-Bus and Openbox; Openbox's autostart starts
   the notifications, the taskbar, the search indexer, the desktop icons
   and a terminal.
6. *Log out* ends the session and the login screen comes back.
   *Power off* sends init `SIGUSR2`: it stops all processes, makes the root
   filesystem read-only (installed system) and powers off.

On an installed system GRUB starts the kernel from `/boot/bzImage` on the
ext4 partition with `root=PARTUUID=... init=/sbin/my-init`, so step 1 loads
no initrd: the kernel mounts the disk itself and the rest is the same.

### System-wide undo

`my-historyd` (root) asks the kernel's **fanotify** for `FAN_CLOSE_WRITE` on the
whole root filesystem, so it hears about every file any program wrote and
closed. It copies each such file into `/var/lib/my-history/store/` as the next
version (up to 50 per file); at boot it saves `/etc`, `/root` and `/home` as
version 0. Versions belong to the file's owner, so users only see their own
files. Paths in `/etc/my-history/exclude` (logs, caches) are skipped.

| Command | Does |
|---|---|
| `my-undo` | undo your last change (run twice = redo) |
| `my-undo list` | recent changes |
| `my-undo history FILE` | versions of FILE |
| `my-undo restore FILE N` | restore version N |
| `my-undo show FILE N` | print version N |

The **File history** window (Start menu, or right-click a file in the file
manager) shows the same with buttons.

### Semantic search

The embedding model **all-MiniLM-L6-v2** (25 MB, run by llama.cpp on the CPU)
turns a text into 384 numbers that describe its meaning. `my-indexd` runs in
each session, cuts the text files in your home folder into ~700-character
sections and stores one vector per section in `~/.cache/my-search/`; other
files are indexed by name. It picks up new, changed and deleted files every
5 seconds. A search turns your query into a vector and ranks files by cosine
similarity, plus a small bonus for exact word matches.

Use the **Search files** window (Super+S) or `my-search what you are looking for`.
The model is English; German text works poorly. PDFs and office files are found
by name only.

## IDE (LazyVim)

*Code editor* on the desktop or in the Start menu opens LazyVim in a terminal
with the JetBrains Mono Nerd Font (for its icons). Press `Space` and wait to
see all shortcuts; `Space e` file tree, `Space Space` find files,
`Space /` search in files, `Space g s` git status,
`:Lazy` plugins, `:Mason` language servers (needs internet).

How it gets there: Buildroot has no Neovim, so `desktop/package/my-ide`
takes the official prebuilt one (x86-64, glibc, like my-distro). During the
build, that same Neovim runs on the build machine, installs LazyVim's
plugins at the versions in `desktop/lazyvim/lazy-lock.json`, and compiles
the tree-sitter parsers with my-distro's cross-compiler. The plugins and
parsers go to `/usr/share/my-ide` once, read-only, for all users; every new
user's home gets the config (`desktop/lazyvim` = `~/.config/nvim`) and links
to them from `/etc/skel`.

my-distro has no C compiler, so `:TSInstall` for more languages doesn't
work on the running system, and `:Lazy update` can't change the shared
plugins; add languages to `desktop/package/my-ide/parsers.lua` and rebuild.

## System updates

An installed my-distro looks for a new release at every login and every
6 hours. When there is one, the taskbar shows **Update to 0.9** next to the
clock; it opens *System update* (also in the Start menu, and a page of
*Users and system*) with the release notes and an **Install update** button.

What happens (`/usr/bin/my-update`, a Python script):

1. It asks GitHub for the latest release
   (`https://api.github.com/repos/dominicdiesahne/my-distro/releases/latest`,
   set in `/etc/my-distro/update.conf`) and compares its version with
   `/etc/my-distro/version`.
2. It downloads the release's **update package** `my-distro-update.tar.xz`
   (the whole root filesystem), its SHA-256 sum and a **signature** of that
   sum, to `/.my-update` on the system disk.
3. It checks the signature with the public key `/etc/my-distro/update-key.pem`
   (Ed25519, `openssl pkeyutl -verify`), then the sum. A package that was not
   signed with the maintainer's private key is refused, even if someone
   tampers with the download.
4. It unpacks the package and moves the new files into place (renames on the
   same disk, so each file is either the old or the new one, never half).
   It **keeps** `/home`, `/root`, `/var`, users and passwords
   (`/etc/passwd`, `shadow`, `group`), the computer name, the keyboard layout,
   the boot loader, the saved Wi-Fi networks and `update.conf`.
5. It runs `/usr/lib/my-distro/installed-fixups` (same as the installer:
   no "Install to disk" icon; it also adds what a new version needs to the
   session files in the home folders, which updates never replace), and
   you restart.

**Undo an update.** Before a file is replaced, the updater keeps the old
one in `/.my-update/previous` (a hard link, so it costs no time). If
something doesn't work after an update, *System update* shows **Undo last
update (back to 0.9.2)**: it puts the old files back and removes the ones
the update added. If the desktop doesn't start any more, log in as root on
the text console (Ctrl+Alt+F1) and run `my-update rollback`. One step back
is kept, until the next update. This works for updates installed by 0.9.2
or newer (the update *to* 0.9.2 is still done by the old updater).

The **live ISO** can't keep an update (it runs from RAM), so there it only
says that a new ISO is out. Systems before 0.8 have no updater: install the
0.8 ISO once, updates come by themselves after that.

From a terminal: `my-update check --notes`, and as root `my-update install`
or `my-update rollback`.

### Making a release

```bash
# 1. Raise the version (the only place it is written down)
echo 0.9 > desktop/overlay/etc/my-distro/version
# 2. Build: out/my-distro.iso and out/my-distro-update.tar.xz
make desktop
# 3. Sum and signature of the update package
make release-files
# 4. Publish all four files
gh release create v0.9 out/my-distro.iso out/my-distro-update.tar.xz* --title "my-distro 0.9" --notes "..."
```

The tag must be the version with a `v` (`v0.9`). The **private signing
key** is `~/.config/my-distro/update-signing-key.pem` on the build machine,
never in the repository. **Back it up**: without it, installed systems can't
get updates any more (a new key needs a new ISO, because the public key is
built into the system). How to make a key is in the `Makefile` at
`release-files`.

## Project layout

| Path | What it is |
|---|---|
| `Makefile` | `make desktop` (ISO and update package), `make release-files` (signature), `make run-iso`; `make` / `make run` for the minimal version |
| `init/init.c` | PID 1 |
| `login/` | Login screen `my-login`, `my-power` (reboot, power off, suspend, eject USB sticks for normal users), `my-admin` (root password check for Users and system) |
| `desktop/overlay/usr/sbin/my-install` | Installer (shell script) |
| `desktop/overlay/usr/bin/my-update` | System updates (Python), settings in `desktop/overlay/etc/my-distro/update.conf`, public key `update-key.pem` |
| `desktop/overlay/etc/my-distro/version` | The version number (the welcome text and `my-update` read it) |
| `panel/` | Taskbar `my-panel` (with volume and battery), `my-screenshot` |
| `settings/my-notifyd.c`, `settings/my-notify.c` | Notifications: the daemon that shows them, and the command that sends one |
| `desktop/overlay/usr/lib/my-distro/automount` | Opens USB sticks and SD cards (started by a udev rule) |
| `desktop/overlay/etc/init.d/S35firewall`, `S45sound`, `S48time` | Firewall, sound start values, time sync |
| `desktop/busybox.fragment` | BusyBox options on top of Buildroot's (ntpd) |
| `desktop/lazyvim/`, `desktop/package/my-ide/` | The IDE: LazyVim config, and how Neovim and the plugins get into the image |
| `settings/` | Settings app, Clock app, setup / Users and system (`my-setup`), and `settings.c` / `dial.c` that the taskbar and login screen share with them |
| `history/` | `my-historyd`, `my-undo`, the File history window |
| `search/` | `my-indexd`, `my-search`, the Search files window |
| `desktop/buildroot.fragment` | Packages and settings on top of Buildroot's `qemu_x86_64_defconfig` |
| `desktop/linux-gui.fragment` | Extra kernel options (graphics, VM drivers, fanotify) |
| `desktop/package/` | How Buildroot builds our programs and downloads the search model |
| `desktop/overlay/` | Files copied into the root filesystem (config, Start menu, launchers) |
| `desktop/post-build.sh` | Last changes to the root filesystem (Openbox shortcuts, user home) |
| `desktop/users.table` | User accounts |
| `desktop/make-wallpaper.py` | Draws the wallpapers, one per theme, and the clock icon (Python + Pillow) |
| `config/`, `rootfs/` | The minimal version |

## Customize

### On the running system

Open **Settings** from the Start menu. Everything is saved to
`~/.config/my-distro/settings`, a short text file you can also edit by hand
(run `my-settings --apply` afterwards for the wallpaper and window colors;
the taskbar and clock notice the change by themselves):

| Setting | Values |
|---|---|
| `theme` | `teal`, `ocean`, `violet`, `ember`, `forest`, `light` |
| `accent` | `#rrggbb`, or empty for the theme's color |
| `wallpaper` | `theme`, `color` (plain), or the path of a picture |
| `wallpaper_mode` | `crop`, `fit`, `center`, `tile`, `stretch` |
| `panel_clock`, `clock_app` | `digital` or `analog` |
| `clock_24h`, `clock_seconds`, `clock_date` | `yes` or `no` |
| `date_format` | `strftime` format, e.g. `%a %d.%m.` |
| `panel_position` | `bottom` or `top` |
| `panel_height`, `font_size` | 28-56 pixels, 8-16 points |

Defaults for all users are in `/etc/my-distro/settings`
(`desktop/overlay/etc/my-distro/settings` in this repository). The login
screen uses the theme and wallpaper of the user it shows. Apps that were
already open when you changed the theme (and GTK apps' light/dark style)
change the next time they start.

### In the build

- **Users and passwords:** the demo account is in `desktop/users.table`
  (format: Buildroot manual, "Users table"), the live root password is
  `BR2_TARGET_GENERIC_ROOT_PASSWD` in `desktop/buildroot.fragment`. A new
  user's home is a copy of `/etc/skel`, made in `desktop/post-build.sh`.
  The setup runs while `/etc/my-distro/setup-done` is missing.
- **Add a program:** find it in Buildroot
  (`make -C ~/my-distro-build/buildroot-* O=~/my-distro-build/desktop-out menuconfig`),
  add its `BR2_PACKAGE_...=y` line to `desktop/buildroot.fragment`, run `make desktop`.
- **Start menu:** `desktop/overlay/etc/my-panel/menu` (`Label|command|icon`).
- **Keyboard layout:** `desktop/overlay/etc/X11/xorg.conf.d/00-keyboard.conf`
  (the default; the setup changes it), the list offered in `settings/my-setup.c`.
- **Wallpapers:** edit `desktop/make-wallpaper.py` and run it.
- **Color themes:** the list in `settings/settings.c`; add a matching entry
  to `THEMES` in `desktop/make-wallpaper.py` for its wallpaper.

## Minimal version

Without Buildroot: kernel, BusyBox and `init.c` only, booting to a shell in
seconds.

```bash
make        # builds the kernel and BusyBox
make run    # boots in the terminal; quit with Ctrl-A, then X
```

## Limitations

- The installer wipes the whole disk: no dual boot next to Windows or
  another Linux.
- NetSurf has limited JavaScript; big web apps (YouTube, Google Docs) don't work.
- Only x86-64. Tested in VirtualBox and QEMU; the drivers for real PCs are
  in, but not yet tried on one (see [On a real PC](#on-a-real-pc)).
- Wi-Fi: no hidden networks and no enterprise Wi-Fi (eduroam) in the
  Wi-Fi window yet.
- Graphics drivers set the screen up, but there is no 3D acceleration
  (Mesa's software renderer only).
- Sound plays on one card at a time (ALSA, no sound server). The digital
  microphones of some new laptops (Intel SOF) don't work yet.
- Closing a laptop's lid does nothing yet; use *Suspend* in the Start menu.

## License

The code in this repository is under the [MIT License](LICENSE).

The built ISO contains many other projects under their own licenses (Linux
and BusyBox under GPL-2.0, Xorg, GTK, Openbox, llama.cpp, ...). Buildroot can
list them and collect their sources: `make -C ~/my-distro-build/buildroot-*
O=~/my-distro-build/desktop-out legal-info`. The search model all-MiniLM-L6-v2
(by sentence-transformers) is Apache-2.0 and downloaded during the build.
