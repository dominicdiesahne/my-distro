"""Two disks with my-distro (0.9.9): python3 t_twodisk.py plain|crypt ISO [BUILD]

An installed disk and the ISO are attached together, like a PC with its own
disk and a my-distro USB stick. GRUB must start the my-distro of the disk it
was started from. Until 0.9.8 the UEFI GRUB took the first disk with
/boot/bzImage: started from the ISO it booted a plain installation, started
from an encrypted installation's EFI partition it booted the ISO (and the
ISO's BIOS GRUB did the same as its UEFI one).

1. installs onto an empty disk (the build's kernel + initrd, as t_install
   does) and gives every entry of the installed boot menu the serial
   console (test only)
2. UEFI (OVMF, Secure Boot off), the installed disk first in the boot
   order, the ISO second: the installed system starts. Logged in, the EFI
   partition's grub.cfg is put back to how 0.9.7/0.9.8 wrote it (no
   configfile) and installed-fixups runs as after an update: the line is
   back (old installations get the fix by themselves)
3. UEFI, the ISO first (as a CD, and as a USB stick: a second hard disk):
   the live system starts
4. BIOS, the ISO first (CD and USB stick): the live system starts
5. BIOS, the installed disk first: the installed system starts

Which system started: the kernel command line on the serial line (rdinit=
is the live system, root=PARTUUID= or my-crypt= the installed one). In
GRUB's menu Down is pressed until the last entry, which has the serial
console in both menus (the ISO's "show boot messages", the installed
"recovery mode" after step 1), then Enter. Screenshots go to SHOTS
(/tmp/vm-twodisk-MODE).

Runs inside WSL, one VM at a time. ISO = out/my-distro.iso of the same build."""
import sys, time, os, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

mode = sys.argv[1]
ISO = os.path.abspath(sys.argv[2])
BUILD = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/my-distro-build-hw')
O = BUILD + '/desktop-out/images'
DISK = BUILD + '/test-twodisk-%s.img' % mode
VARS = '/tmp/vm-twodisk-vars-%s.fd' % mode
SHOTS = '/tmp/vm-twodisk-' + mode
PASS = 'testpass1234'
os.makedirs(SHOTS, exist_ok=True)
results = []

def record(name, ok, detail=''):
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + ('\n    ' + detail.strip()[-600:].replace('\n', '\n    ') if detail else ''),
          flush=True)

def check(vm, name, cmd, want, timeout=300, absent=None):
    out = vm.run(cmd, timeout).replace('\r', '')
    body = out.split('\n', 1)[1] if '\n' in out else out
    ok = want in body and (absent is None or absent not in body)
    record(name, ok, body)
    return body

def login(vm, timeout=2400):
    vm.expect(r'login: ', timeout)
    vm.send('root\n'); vm.expect('Password: ', 60); vm.send('root\n'); vm.expect(r'# ', 120)
    vm.send('stty cols 200; export PS1="# "\n'); vm.expect(r'# ', 30)

def poweroff(vm):
    vm.run('sync', 60)
    vm.send('poweroff\n')
    try:
        vm.proc.wait(180)
    except subprocess.TimeoutExpired:
        pass
    vm.quit()

base = ['-m', '2048', '-smp', '4', '-vga', 'std', '-nic', 'user,model=e1000']
uefi = ['-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
        '-drive', 'if=pflash,format=raw,file=' + VARS]

def drives(disk_first, usb=False):
    """the installed disk (virtio) and the ISO: an IDE CD-ROM, or (usb) a
    second hard disk, as the ISO written to a USB stick is; bootindex gives
    the firmware's boot order (OVMF and SeaBIOS both follow it)"""
    d, c = (1, 2) if disk_first else (2, 1)
    iso = (['-drive', 'file=%s,if=none,id=cd0,readonly=on,format=raw' % ISO,
            '-device', 'virtio-blk-pci,drive=cd0,bootindex=%d' % c] if usb else
           ['-drive', 'file=%s,if=none,id=cd0,media=cdrom,readonly=on,format=raw' % ISO,
            '-device', 'ide-cd,drive=cd0,bootindex=%d' % c])
    return ['-drive', 'file=%s,if=none,id=hd0,format=raw' % DISK,
            '-device', 'virtio-blk-pci,drive=hd0,bootindex=%d' % d] + iso

def start(tag, args, want):
    """boots, picks GRUB's last entry, returns (vm, the kernel command line);
    want = 'live' or 'installed'"""
    if VARS in ' '.join(args):                 # UEFI: a fresh NVRAM each time
        subprocess.run(['cp', '/usr/share/OVMF/OVMF_VARS_4M.fd', VARS], check=True)
    vm = VM('twodisk-%s-%s' % (mode, tag), args)
    # GRUB waits 3 s (installed) or 5 s (ISO): keep pressing Down from before
    # its menu appears; it stops the countdown and stays on the last entry
    end = time.time() + 240
    shot_at = time.time() + 60
    while time.time() < end:
        vm.keys('down')
        time.sleep(2)
        if time.time() > shot_at:
            vm.shot('%s/%s-menu.png' % (SHOTS, tag)); shot_at = end + 1
    vm.shot('%s/%s-menu-last.png' % (SHOTS, tag))
    vm.keys('ret')
    try:
        out = vm.expect(r'Command line: [^\r\n]*', 900)
        line = re.findall(r'Command line: [^\r\n]*', out)[-1]
    except (TimeoutError, RuntimeError) as e:
        vm.shot('%s/%s-no-kernel.png' % (SHOTS, tag))
        record('%s: a kernel started with the serial console' % tag, False, str(e))
        return vm, ''
    live = 'rdinit=' in line
    installed = 'root=PARTUUID=' in line or 'my-crypt=' in line
    ok = live if want == 'live' else installed and not live
    record('%s: the %s system starts' % (tag, want), ok, line)
    return vm, line

# --- 1. install -----------------------------------------------------------------
if os.path.exists(DISK): os.remove(DISK)
subprocess.run(['truncate', '-s', '12G', DISK], check=True)
vm = VM('twodisk-inst-' + mode, base + ['-drive', 'file=%s,if=virtio,format=raw' % DISK,
        '-kernel', O + '/bzImage', '-initrd', O + '/rootfs.cpio.gz',
        '-append', 'rdinit=/sbin/my-init initramfs_options=size=90% console=tty1 console=ttyS0,115200'])
login(vm)
vm.send('/usr/sbin/my-install\n')
vm.expect(r'Number \(1-\d\): ', 300); vm.send('1\n')
vm.expect(r'Type yes or no \[no\]: ', 60)
if mode == 'crypt':
    vm.send('yes\n')
    vm.expect('Passphrase: ', 60); vm.send(PASS + '\n')
    vm.expect('Passphrase again: ', 60); vm.send(PASS + '\n')
else:
    vm.send('no\n')
vm.expect('Type yes to continue: ', 60); vm.send('yes\n')
out = vm.expect(r'(Press Enter to close this window\.)', 3600)
record('install finished', 'Done. my-distro is installed' in out, out[-600:])
vm.send('\n'); vm.expect(r'# ', 60)
if mode == 'crypt':
    pre = 'printf %s ' + PASS + ' | cryptsetup open --key-file=- /dev/vda3 t; mkdir -p /mnt/e; mount /dev/vda2 /mnt/e; '
    menu, post = '/mnt/e/grub/grub.cfg', '; umount /mnt/e; cryptsetup close t'
else:
    pre = 'mkdir -p /mnt/e /mnt/r; mount /dev/vda3 /mnt/r; mount /dev/vda2 /mnt/e; '
    menu, post = '/mnt/r/boot/grub/grub.cfg', '; umount /mnt/e /mnt/r'
check(vm, 'installed menu: serial console on every entry (test only)',
      pre + "sed -i 's/console=tty1/console=tty1 console=ttyS0,115200/' " + menu + '; grep -c ttyS0 ' + menu +
      '; cat /mnt/e/EFI/BOOT/grub.cfg' + post, 'configfile', 120)
poweroff(vm)

# --- 2. UEFI, installed disk first -----------------------------------------------
vm, line = start('uefi-disk-first', uefi + base + drives(True), 'installed')
if line:
    if mode == 'crypt':
        try:
            vm.expect('Enter passphrase for', 900); vm.send(PASS + '\n')
        except TimeoutError as e:
            record('passphrase prompt', False, str(e))
    try:
        login(vm)
        check(vm, 'running from the disk, not the live system', 'cat /proc/cmdline', 'init=/sbin/my-init',
              absent='rdinit=')
        # the EFI partition's grub.cfg as 0.9.7 and 0.9.8 wrote it, then an update's installed-fixups
        check(vm, "grub.cfg of 0.9.8 (no configfile)",
              'E=$(/usr/lib/my-distro/esp-mount); sed -i "/configfile/d; /0\\.9\\.9/d" $E/EFI/BOOT/grub.cfg; '
              'cat $E/EFI/BOOT/grub.cfg; grep -c configfile $E/EFI/BOOT/grub.cfg', '\n0')
        check(vm, 'installed-fixups (as after an update) adds configfile',
              '/usr/lib/my-distro/installed-fixups / >/dev/null 2>&1; cat /boot/efi/EFI/BOOT/grub.cfg',
              'configfile $prefix/grub.cfg', 600)
        poweroff(vm)
    except (TimeoutError, RuntimeError) as e:
        record('installed system logged in', False, str(e))
        vm.shot(SHOTS + '/uefi-disk-first-login.png'); vm.quit()
else:
    vm.quit()

# --- 3.-5. -------------------------------------------------------------------------
for tag, args, want in (('uefi-iso-first', uefi + base + drives(False), 'live'),
                        ('uefi-usb-first', uefi + base + drives(False, usb=True), 'live'),
                        ('bios-iso-first', base + drives(False), 'live'),
                        ('bios-usb-first', base + drives(False, usb=True), 'live'),
                        ('bios-disk-first', base + drives(True), 'installed')):
    vm, line = start(tag, args, want)
    vm.quit()

print('\n== SUMMARY twodisk ' + mode)
for n, ok in results:
    print(('PASS ' if ok else 'FAIL ') + n)
