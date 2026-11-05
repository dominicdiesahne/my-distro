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

![LazyVim IDE with file explorer and syntax highlighting](docs/screenshots/ide.png)

## Features

- **Own init in C** (`init/init.c`) as PID 1: mounts filesystems, runs the boot
  scripts, starts a login on the console, reaps zombies, reboots/powers off.
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
  Right-click, *Create New…* makes text, Markdown, shell, Python, C, HTML,
  CSS, JavaScript, JSON, CSV, Lua, Makefile and config files from templates
  (`~/Templates`).
- **Python 3** with pip, so `.py` scripts run (`python3 file.py`).
- **Live ISO** for BIOS and UEFI; tested in VirtualBox and QEMU.
- **Installer** (`my-install`): copies the live system onto a hard disk with
  GRUB for BIOS and UEFI, so your files and settings survive a reboot.

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

The result is **`out/my-distro.iso`** (~230 MB). Build files go to
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

### 3. Log in

| User | Password | |
|---|---|---|
| `dominic` | `dominic` | normal user, pre-filled on the login screen |
| `root` | `root` | administrator |

These are default passwords of a **live system for testing**: everything runs
in RAM and is gone after a reboot, unless you [install it](#4-install-to-a-disk-optional).
The installer asks for new passwords; or change them with `passwd`.

### 4. Install to a disk (optional)

The live system forgets everything when it shuts down. To keep your files,
install it onto a hard disk:

1. Give the VM a hard disk: **VirtualBox** Settings, Storage, add a hard disk
   (8 GB is plenty, dynamically allocated). In QEMU: `make run-install`.
2. Boot the ISO and log in, then double-click **Install to disk** on the
   desktop (or Start menu, *Install to disk*; or `my-install` as root).
   It asks for the root password (`root`).
3. Pick the disk, type `yes`. **Everything on that disk is deleted.**
   It copies the system (a minute or two) and installs GRUB, then asks
   for new passwords for `root` and `dominic`.
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
| Alt+Tab, Alt+F4 | Switch / close windows |
| Ctrl+Alt+F1 / Ctrl+Alt+F7 | Text console / back to the desktop |

The keyboard layout is German (`desktop/overlay/etc/X11/xorg.conf.d/00-keyboard.conf`).

## How it works

### Boot

1. GRUB loads the kernel and the whole root filesystem into RAM (initrd).
2. The kernel starts our init, `/sbin/my-init` (`init/init.c`).
3. init mounts `/proc`, `/sys`, `/dev`, `/tmp` and runs `/etc/init.d/rcS`:
   udev, network (DHCP), `S50history` (file history), `S90desktop`.
4. `S90desktop` starts the X server with **`my-login`** as its only client.
5. After login, `my-login` drops root rights and runs `~/.xinitrc` as you,
   which starts Openbox; Openbox's autostart starts the taskbar, the search
   indexer, the desktop icons and a terminal.
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
the tree-sitter parsers with my-distro's cross-compiler. The result is put
into dominic's home. The config is `desktop/lazyvim` (= `~/.config/nvim`).

my-distro has no C compiler, so `:TSInstall` for more languages doesn't
work on the running system; add them to `desktop/package/my-ide/parsers.lua`
and rebuild. The IDE is set up for the user `dominic` only.

## Project layout

| Path | What it is |
|---|---|
| `Makefile` | `make desktop` (ISO), `make run-iso`; `make` / `make run` for the minimal version |
| `init/init.c` | PID 1 |
| `login/` | Login screen `my-login`, `my-power` (reboot/power off for normal users) |
| `desktop/overlay/usr/sbin/my-install` | Installer (shell script) |
| `panel/` | Taskbar `my-panel` |
| `desktop/lazyvim/`, `desktop/package/my-ide/` | The IDE: LazyVim config, and how Neovim and the plugins get into the image |
| `settings/` | Settings app, Clock app, and `settings.c` / `dial.c` that the taskbar and login screen share with them |
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

- **Users and passwords:** `desktop/users.table` (format: Buildroot manual,
  "Users table"), root password `BR2_TARGET_GENERIC_ROOT_PASSWD` in
  `desktop/buildroot.fragment`. The user's home is set up in `desktop/post-build.sh`.
- **Add a program:** find it in Buildroot
  (`make -C ~/my-distro-build/buildroot-* O=~/my-distro-build/desktop-out menuconfig`),
  add its `BR2_PACKAGE_...=y` line to `desktop/buildroot.fragment`, run `make desktop`.
- **Start menu:** `desktop/overlay/etc/my-panel/menu` (`Label|command|icon`).
- **Keyboard layout:** `desktop/overlay/etc/X11/xorg.conf.d/00-keyboard.conf`.
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
  another Linux, and no updates of an installed system (reinstall instead).
- NetSurf has limited JavaScript; big web apps (YouTube, Google Docs) don't work.
- Only x86-64; tested in VirtualBox and QEMU, not on real hardware.
- The ISO is about 220 MB, half of it the IDE (Neovim, plugins, parsers).

## License

The code in this repository is under the [MIT License](LICENSE).

The built ISO contains many other projects under their own licenses (Linux
and BusyBox under GPL-2.0, Xorg, GTK, Openbox, llama.cpp, ...). Buildroot can
list them and collect their sources: `make -C ~/my-distro-build/buildroot-*
O=~/my-distro-build/desktop-out legal-info`. The search model all-MiniLM-L6-v2
(by sentence-transformers) is Apache-2.0 and downloaded during the build.
