# my-distro

A small Linux distribution built from source, with its own init system,
desktop pieces and two features most desktops don't have: **system-wide undo**
for every file change and **AI semantic file search** that finds files by
meaning. It boots as a live ISO in a VM.

Most of the parts that make it *this* distro are written in plain C and kept
short and commented, so you can read how an OS fits together: `init` (PID 1),
the taskbar, the login screen, the file history daemon and the search indexer.

![Desktop with taskbar, Start menu and app icons](docs/screenshots/desktop.png)

| Login screen | Semantic search |
|---|---|
| ![Login screen](docs/screenshots/login.png) | ![Search files](docs/screenshots/search.png) |
| **File history (undo)** | **Web browser** |
| ![File history](docs/screenshots/history.png) | ![NetSurf browser](docs/screenshots/browser.png) |

## Features

- **Own init in C** (`init/init.c`) as PID 1: mounts filesystems, runs the boot
  scripts, starts a login on the console, reaps zombies, reboots/powers off.
- **Graphical login** (`login/my-login.c`): checks passwords against
  `/etc/shadow`, drops root rights and starts your desktop as you.
- **Taskbar** (`panel/my-panel.c`): Start menu with icons, a button per window
  with the app's icon, clock.
- **System-wide undo** (`history/`): every file that any program changes is
  saved as a version (kernel *fanotify*). Undo from a window or with `my-undo`.
- **Semantic file search** (`search/`): an embedding model (all-MiniLM-L6-v2 via
  llama.cpp) indexes your files by meaning. *"customer agreement for training
  subscription"* finds a note about a client signing up for a gym membership,
  without a single shared word.
- **Desktop:** Xorg, Openbox, PCManFM file manager with desktop icons, NetSurf
  web browser (HTTPS), xterm, nano, htop, calculator, generated wallpaper.
- **Live ISO** for BIOS and UEFI; tested in VirtualBox and QEMU.

## Try it

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

The result is **`out/my-distro.iso`** (~90 MB). Build files go to
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
in RAM and is gone after a reboot. Change the users before you use it anywhere
else (see [Customize](#customize)).

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
   *Power off* sends init `SIGUSR2`: it stops all processes and powers off.

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

## Project layout

| Path | What it is |
|---|---|
| `Makefile` | `make desktop` (ISO), `make run-iso`; `make` / `make run` for the minimal version |
| `init/init.c` | PID 1 |
| `login/` | Login screen `my-login`, `my-power` (reboot/power off for normal users) |
| `panel/` | Taskbar `my-panel` |
| `history/` | `my-historyd`, `my-undo`, the File history window |
| `search/` | `my-indexd`, `my-search`, the Search files window |
| `desktop/buildroot.fragment` | Packages and settings on top of Buildroot's `qemu_x86_64_defconfig` |
| `desktop/linux-gui.fragment` | Extra kernel options (graphics, VM drivers, fanotify) |
| `desktop/package/` | How Buildroot builds our programs and downloads the search model |
| `desktop/overlay/` | Files copied into the root filesystem (config, Start menu, launchers) |
| `desktop/post-build.sh` | Last changes to the root filesystem (Openbox shortcuts, user home) |
| `desktop/users.table` | User accounts |
| `desktop/make-wallpaper.py` | Draws the wallpaper (Python + Pillow) |
| `config/`, `rootfs/` | The minimal version |

## Customize

- **Users and passwords:** `desktop/users.table` (format: Buildroot manual,
  "Users table"), root password `BR2_TARGET_GENERIC_ROOT_PASSWD` in
  `desktop/buildroot.fragment`. The user's home is set up in `desktop/post-build.sh`.
- **Add a program:** find it in Buildroot
  (`make -C ~/my-distro-build/buildroot-* O=~/my-distro-build/desktop-out menuconfig`),
  add its `BR2_PACKAGE_...=y` line to `desktop/buildroot.fragment`, run `make desktop`.
- **Start menu:** `desktop/overlay/etc/my-panel/menu` (`Label|command|icon`).
- **Keyboard layout:** `desktop/overlay/etc/X11/xorg.conf.d/00-keyboard.conf`.
- **Wallpaper:** edit `desktop/make-wallpaper.py` and run it.

## Minimal version

Without Buildroot: kernel, BusyBox and `init.c` only, booting to a shell in
seconds.

```bash
make        # builds the kernel and BusyBox
make run    # boots in the terminal; quit with Ctrl-A, then X
```

## Limitations

- Live system only: no installer, everything is lost on reboot.
- NetSurf has limited JavaScript; big web apps (YouTube, Google Docs) don't work.
- Only x86-64; tested in VirtualBox and QEMU, not on real hardware.

## License

The code in this repository is under the [MIT License](LICENSE).

The built ISO contains many other projects under their own licenses (Linux
and BusyBox under GPL-2.0, Xorg, GTK, Openbox, llama.cpp, ...). Buildroot can
list them and collect their sources: `make -C ~/my-distro-build/buildroot-*
O=~/my-distro-build/desktop-out legal-info`. The search model all-MiniLM-L6-v2
(by sentence-transformers) is Apache-2.0 and downloaded during the build.
