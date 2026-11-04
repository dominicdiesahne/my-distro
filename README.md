# my-distro

A tiny Linux distro: the Linux kernel, BusyBox for the shell and commands,
and a hand-written `init` in C as PID 1. It boots in QEMU to a shell.

## Layout

| Path | What it is |
|---|---|
| `Makefile` | Downloads and builds everything, `make run` boots it |
| `init/init.c` | PID 1: mounts filesystems, starts the shell, reaps zombies, powers off |
| `config/kernel.fragment` | Kernel options added on top of the x86_64 defconfig |
| `rootfs/` | Files copied into the root filesystem (`/etc/...`) |

Build output goes to `~/my-distro-build` inside WSL, not into this folder.

## Build and run (inside Kali WSL)

One-time setup:

```bash
sudo apt install build-essential bc bison flex libelf-dev libssl-dev libncurses-dev cpio wget qemu-system-x86
```

Then, from this folder:

```bash
make        # first run takes a while: it compiles the kernel
make run    # boot; quit QEMU with Ctrl-A then X, or type `poweroff`
```

After changing `init.c` or `rootfs/`, `make run` only rebuilds the initramfs (seconds).

## How it boots

1. QEMU loads the kernel (`bzImage`) and the initramfs (a cpio archive of `rootfs`).
2. The kernel unpacks the initramfs into RAM and runs `/init`, our C program, as PID 1.
3. `init` mounts `/proc`, `/sys`, `/dev`, `/tmp`, sets the hostname and starts `/bin/sh`.
4. `poweroff` (BusyBox) sends `init` SIGUSR2; `init` stops all processes and powers off.

## Desktop version (Xorg + Openbox)

`make desktop` builds a second, full system with [Buildroot](https://buildroot.org):
kernel, glibc, Xorg, the Openbox window manager, xterm, a clock, a calculator
and DejaVu fonts, all compiled from source. Your `init.c` is still PID 1
(Buildroot builds it as the package `my-init`).

```bash
make desktop       # first build: ~1 GB download, about an hour
make run-desktop   # QEMU window shows the desktop, this terminal shows the serial shell
```

Boot order: kernel → `/sbin/my-init` → `/etc/init.d/rcS` (starts udev, network,
then `S90desktop`) → `xinit` starts the X server with **`my-login`**, the login
screen → after login it runs `~/.xinitrc` as that user → Openbox → its autostart
(`~/.config/openbox/autostart`) starts the taskbar, the desktop icons and a terminal.
Log out (Start menu) ends the session and the login screen comes back.

**Users** (live system: password changes are lost on reboot):

| User | Password | |
|---|---|---|
| `dominic` | `dominic` | normal user, filled in on the login screen |
| `root` | `root` | administrator |

- **Login screen** (`login/my-login.c`, our own C program): checks the password
  against the SHA-256 hash in `/etc/shadow` with `crypt()`, then drops root rights
  (`initgroups`, `setgid`, `setuid`) and starts the user's desktop. When the
  session ends it stops everything the user left running.
- **`my-power`** (`login/my-power.c`): a tiny setuid-root program, so normal users
  can `my-power reboot` / `my-power poweroff` (only root may signal init).
- **Text console** (Ctrl+Alt+F1): `init` now starts `login` instead of a root
  shell when the system has passwords (`/etc/shadow`).
**File history (system-wide undo):** every change to a file is saved as a version.

- `my-historyd` (`history/my-historyd.c`, started by `/etc/init.d/S50history`)
  uses the kernel's **fanotify** to get an event whenever any program closes a
  file it wrote, anywhere on the root filesystem, and copies the file into
  `/var/lib/my-history/store/`. At boot it saves the original state of `/etc`,
  `/root` and `/home` as version 0, so even the first change can be undone.
  Skipped: `/etc/my-history/exclude` (logs, caches). Up to 50 versions per file.
- Versions belong to the file's owner: users see and restore only their own
  files, root sees everything.
- **File history** app (Start menu, or right-click a file in the file manager):
  live list of changes, *Undo this change*, *Undo last change*, *View*.
- Command line:

  | Command | Does |
  |---|---|
  | `my-undo` | undo your most recent change |
  | `my-undo list` | recent changes |
  | `my-undo history FILE` | all versions of FILE |
  | `my-undo restore FILE N` | put version N back |
  | `my-undo show FILE N` | print version N |

  Undoing is a change too, so it is saved as well: undo twice = redo.
- Like everything on the live system, the history is in RAM and gone after a reboot.

- Users are defined in `desktop/users.table`; the root password is
  `BR2_TARGET_GENERIC_ROOT_PASSWD` in `desktop/buildroot.fragment`.

What's on the desktop:

- **Taskbar** (`my-panel`, our own C program in `panel/my-panel.c`): Start menu,
  one button per window with the app's icon (click = bring to front, click
  again = minimise), clock. The Start menu entries and their icons are in
  `/etc/my-panel/menu`. Apps that don't send their own icon (xterm, xcalc) get
  one from `/etc/my-panel/icons`. Icons come from the Adwaita theme and are
  drawn with Imlib2.
- **Wallpaper**: `desktop/overlay/usr/share/backgrounds/my-distro.png`, drawn by
  `desktop/make-wallpaper.py` (Python + Pillow). Change the script and run
  `python make-wallpaper.py overlay/usr/share/backgrounds/my-distro.png`.
- **File manager** (PCManFM): double-click folders to open them, text files open
  in the text editor. It also draws the desktop icons.
- **Web browser** (NetSurf): small and fast, HTTPS works. Its JavaScript
  support is limited, so big web apps (YouTube, Google Docs) don't work.
  Home page: DuckDuckGo's no-JavaScript version.
- **Apps**: terminal (xterm), text editor (nano), system monitor (htop), calculator, clock.

| Shortcut | Does |
|---|---|
| Super+Space or Alt+F1 | Open the Start menu |
| Super+T or Ctrl+Alt+T | Terminal |
| Super+E | File manager |
| Super+Up / Super+Down | Maximise / minimise the window |
| Alt+Tab | Switch windows |
| Alt+F4 | Close the window |

| Path | What it is |
|---|---|
| `panel/my-panel.c` | The taskbar (Xlib + Xft) |
| `desktop/buildroot.fragment` | Packages and settings on top of Buildroot's `qemu_x86_64_defconfig` |
| `desktop/linux-gui.fragment` | Kernel options for graphics, keyboard, mouse |
| `desktop/package/` | Tells Buildroot how to build `init/init.c` and `panel/my-panel.c` |
| `desktop/overlay/` | Files copied into the root filesystem: desktop start script, `.xinitrc`, Openbox autostart, Start menu, desktop icons, file manager settings, keyboard layout |
| `desktop/post-build.sh` | Last changes to the root filesystem: Openbox shortcuts and theme |

To add a program, find it in Buildroot (`make -C ~/my-distro-build/buildroot-* O=~/my-distro-build/desktop-out menuconfig`),
add its `BR2_PACKAGE_...=y` line to `desktop/buildroot.fragment` and run `make desktop` again.
Rebuilds only compile what changed.

## Bootable ISO for a VM

`make desktop` also builds **`out/my-distro.iso`**: a live system that boots on
BIOS and UEFI. GRUB loads the kernel and the whole root filesystem into RAM,
then the kernel starts our init (`rdinit=/sbin/my-init`). Changes are lost on
reboot. Test it with `make run-iso`.

**VirtualBox:** New VM → Type *Linux*, Version *Other Linux (64-bit)*. It must be
a **64-bit** type: with "Other/Unknown" VirtualBox hides the CPU's 64-bit mode
and the kernel stops before it can print anything (black screen) →
2048 MB RAM, 2 CPUs → no hard disk needed → Settings → Storage → put
`my-distro.iso` in the optical drive → Display → Graphics Controller *VMSVGA* → Start.

**VMware Workstation Player:** Create a new VM → "Installer disc image file (iso)"
→ *Linux, Other Linux 6.x kernel 64-bit* → 2 GB RAM → Finish.

**Hyper-V:** Generation 2 VM, 2048 MB RAM, **Secure Boot off** (Settings →
Security), DVD drive with the ISO.

Boot menu: pick **safe graphics** if the screen stays black. In the VM,
Ctrl+Alt+F1 shows the text shell, Ctrl+Alt+F7 goes back to the desktop.

## Ideas for next steps

- Type `exit` in the shell and watch init start a new one. Then change that behaviour.
- Replace BusyBox's `sh` with your own shell in C (`fork`, `execve`, `pipe`, `dup2`).
- Add a `/etc/inittab`-style config file that init reads.
- Make a bootable ISO with GRUB (`grub-mkrescue`).
