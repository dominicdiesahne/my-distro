"""Installer tests. python3 t_install.py plain|crypt
1. boots the live system (kernel + initrd, serial root shell) with an empty
   8 GB disk and runs my-install on the serial line
2. points the installed boot menu's first entry at the serial console too
   (test only), boots the disk with BIOS (GRUB), and for "crypt" types the
   passphrase at the initramfs prompt
3. checks swap file, resume, Hibernate menu; hibernates and boots again:
   the marker process must still be there (resumed, not a fresh boot)
4. installed-fixups as after an update: the ESP gets a new initramfs (crypt)
5. (0.9.7) the EFI partition has shim, GRUB and its grub.cfg, and the disk
   also starts with UEFI (OVMF, Secure Boot off) through shim

(0.9.9) GRUB of the installed system also writes to the serial line (test
only: lines put at the top of its boot menu), so every boot from the disk
is checked in steps: GRUB read its boot menu, then the kernel or the
passphrase prompt came. If a step doesn't come in time, the test stops at
once with screenshots of the screen and of GRUB's "set", "ls" and its boot
menu (typed on the VGA console) in SHOTS, instead of waiting 40 minutes.
(Once, a BIOS boot of the encrypted disk sat at a GRUB prompt for 25 min.)

Runs inside WSL: python3 tests/t_install.py plain|crypt [BUILD]"""
import sys, time, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

mode = sys.argv[1]
BUILD = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/my-distro-build-hw')
O = BUILD + '/desktop-out/images'
DISK = BUILD + '/test-%s.img' % mode
PASS = 'testpass1234'
SHOTS = '/tmp/vm-install-shots-' + mode
GRUB_MARK = 'MY-GRUB-READ-ITS-MENU'
# Put at the top of the installed boot menu (test only): GRUB's output also
# goes to the serial line, and it says that it read this file
GRUB_SERIAL = ('serial --unit=0 --speed=115200\\nterminal_output --append serial\\n'
               'echo ' + GRUB_MARK + ' root=\\$root prefix=\\$prefix\\n')
results = []

def summary():
    print('\n== SUMMARY ' + mode)
    for n, ok in results:
        print(('PASS ' if ok else 'FAIL ') + n)

def boot_failed(v, tag, what):
    """A boot step didn't come: what the screen shows, and what GRUB knows
    (if it sits at its prompt or menu: c opens the prompt), then stop"""
    os.makedirs(SHOTS, exist_ok=True)
    v._pump()
    print('FAIL %s (%s); serial line so far:\n%s' % (what, tag, v.buf[-3000:]), flush=True)
    print('screenshot', v.shot('%s/%s-1-screen.png' % (SHOTS, tag)), flush=True)
    v.keys('c'); time.sleep(3)
    for i, cmd in enumerate(('set', 'ls', 'cat $prefix/grub.cfg')):
        v.type(cmd + '\n'); time.sleep(8)
        print('screenshot', v.shot('%s/%s-%d-%s.png' % (SHOTS, tag, i + 2, cmd.split()[0])), flush=True)
    v._pump(); print(v.buf[-3000:], flush=True)
    results.append((what + ' (' + tag + ')', False))
    summary()
    v.quit()
    sys.exit(1)

def boot_steps(v, tag):
    """GRUB read the installed boot menu, then (crypt) the passphrase prompt"""
    try:
        v.expect(GRUB_MARK, 900)
    except (TimeoutError, RuntimeError):
        boot_failed(v, tag, 'GRUB read the installed boot menu')
    if mode == 'crypt':
        try:
            v.expect('Enter passphrase for', 1500)
        except (TimeoutError, RuntimeError):
            boot_failed(v, tag, 'kernel and initramfs asked for the passphrase')
        v.send(PASS + '\n')

def check(vm, name, cmd, want, timeout=300):
    out = vm.run(cmd, timeout)
    ok = want in out
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + '\n    ' + out.strip()[-500:].replace('\n', '\n    '), flush=True)

def login(vm, timeout=2400):
    vm.expect(r'login: ', timeout)
    vm.send('root\n'); vm.expect('Password: ', 60); vm.send('root\n'); vm.expect(r'# ', 120)
    vm.send('stty cols 200; export PS1="# "\n'); vm.expect(r'# ', 30)

if os.path.exists(DISK): os.remove(DISK)
subprocess.run(['truncate', '-s', '12G', DISK], check=True)
common = ['-m', '2048', '-smp', '4', '-vga', 'std', '-nic', 'user,model=e1000',
          '-drive', 'file=%s,if=virtio,format=raw' % DISK]

# --- 1. install -------------------------------------------------------------
vm = VM('inst-' + mode, common + ['-kernel', O + '/bzImage', '-initrd', O + '/rootfs.cpio.gz',
        '-append', 'rdinit=/sbin/my-init initramfs_options=size=90% console=tty1 console=ttyS0,115200'])
t0 = time.time(); login(vm); print('live up after %d s' % (time.time() - t0), flush=True)
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
print(out[-1500:], flush=True)
results.append(('install finished', 'Done. my-distro is installed' in out))
vm.send('\n'); vm.expect(r'# ', 60)
print(vm.run('tail -20 /tmp/my-install.log', 60), flush=True)
# serial console for the installed system (test only)
if mode == 'crypt':
    print(vm.run('printf %s ' + PASS + ' | cryptsetup open --key-file=- /dev/vda3 t; mkdir -p /mnt/e; mount /dev/vda2 /mnt/e; '
           "sed -i 's/console=tty1 quiet/console=tty1 console=ttyS0,115200/' /mnt/e/grub/grub.cfg; "
           '{ printf "' + GRUB_SERIAL + '"; cat /mnt/e/grub/grub.cfg; } > /mnt/e/grub/grub.new && mv /mnt/e/grub/grub.new /mnt/e/grub/grub.cfg; '
           'head -4 /mnt/e/grub/grub.cfg; grep -m2 linux /mnt/e/grub/grub.cfg; ls -la /mnt/e/my-distro; '
           'cat /mnt/e/EFI/BOOT/grub.cfg; umount /mnt/e; cryptsetup close t', 120), flush=True)
else:
    print(vm.run('mkdir -p /mnt/r; mount /dev/vda3 /mnt/r; '
           "sed -i 's/console=tty1 quiet/console=tty1 console=ttyS0,115200/' /mnt/r/boot/grub/grub.cfg; "
           '{ printf "' + GRUB_SERIAL + '"; cat /mnt/r/boot/grub/grub.cfg; } > /mnt/r/boot/grub/grub.new && mv /mnt/r/boot/grub/grub.new /mnt/r/boot/grub/grub.cfg; '
           'head -4 /mnt/r/boot/grub/grub.cfg; grep -m2 linux /mnt/r/boot/grub/grub.cfg; ls -la /mnt/r/swapfile; umount /mnt/r', 120), flush=True)
# (sync and a clean power-off before QEMU quits: the next VM reads this disk)
vm.run('sync', 60)
vm.send('poweroff\n')
try:
    vm.proc.wait(120)
except subprocess.TimeoutExpired:
    pass
vm.quit()

# --- 2. boot from the disk ------------------------------------------------------
def boot_disk(tag):
    v = VM('disk-%s-%s' % (mode, tag), common)
    boot_steps(v, 'bios-' + tag)
    return v

vm = boot_disk('1')
t0 = time.time(); login(vm); print('installed system up after %d s' % (time.time() - t0), flush=True)
# (the kernel mounts root= itself and calls it /dev/root; the command line says which)
check(vm, 'root filesystem', 'mount | grep " / "; cat /proc/cmdline', 'mapper' if mode == 'crypt' else 'root=')
check(vm, 'swap file on', 'cat /proc/swaps', '/swapfile')
check(vm, 'resume device set', 'cat /sys/power/resume', ':')
check(vm, 'Hibernate in the Start menu', 'grep Hibernate /etc/my-panel/menu', 'my-hibernate')
check(vm, 'my-power hibernate --check', 'my-power hibernate --check', 'possible')
if mode == 'crypt':
    check(vm, 'crypt config', 'cat /etc/my-distro/crypt', 'luks_uuid')
    # what an update does at the end: installed-fixups makes the initramfs again
    check(vm, 'installed-fixups after an update', '/usr/lib/my-distro/installed-fixups / 2>&1 | tail -3', 'on the EFI partition')
# hibernate with a marker process that only survives a resume
vm.run('sleep 99999 & echo $! > /tmp/marker; cat /tmp/marker', 30)
vm.send('my-power hibernate\n')
try:
    vm.proc.wait(900)
    results.append(('machine switched off for hibernation', True))
except subprocess.TimeoutExpired:
    results.append(('machine switched off for hibernation', False))
    print(vm.buf[-2000:]); vm.quit()
vm.quit()

vm = boot_disk('2')
time.sleep(5)
try:
    vm.expect(r'(PM: hibernation: hibernation exit|Restarting tasks|# )', 2400)
    vm.send('\n'); vm.expect(r'# ', 300)
    check(vm, 'resumed (marker process alive)', 'kill -0 $(cat /tmp/marker) && echo ALIVE', 'ALIVE')
except TimeoutError as e:
    print('no resume:', e); results.append(('resumed (marker process alive)', False))
check(vm, 'dmesg says restored', 'dmesg | grep -i -E "hibernat|restor|image" | tail -5', 'mage')
# 0.9.7: what my-install put on the EFI partition for UEFI and Secure Boot
check(vm, 'EFI partition: shim, GRUB, MokManager, certificate, grub.cfg',
      '/usr/lib/my-distro/esp-mount && ls /boot/efi/EFI/BOOT/ | tr "\\n" " "; cat /boot/efi/EFI/BOOT/grub.cfg',
      'grubx64.efi')
# 0.9.9: that grub.cfg goes on to this installation's own boot menu
check(vm, "EFI grub.cfg: this filesystem's ID, then configfile",
      'grep -E "fs-uuid|configfile" /boot/efi/EFI/BOOT/grub.cfg', 'configfile $prefix/grub.cfg')
check(vm, 'my-secureboot refresh (after an update)', 'my-secureboot refresh && echo REFRESHED', 'REFRESHED')
check(vm, 'refresh leaves a current grub.cfg alone', 'grep -c configfile /boot/efi/EFI/BOOT/grub.cfg', '\n1')
vm.run('sync', 60)
vm.send('poweroff\n')
try:
    vm.proc.wait(180)
except subprocess.TimeoutExpired:
    pass
vm.quit()

# --- 3. the same disk with UEFI (OVMF, Secure Boot off): shim -> GRUB -> kernel --
VARS = '/tmp/vm-ovmf-vars-%s.fd' % mode
subprocess.run(['cp', '/usr/share/OVMF/OVMF_VARS_4M.fd', VARS], check=True)
uefi = ['-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
        '-drive', 'if=pflash,format=raw,file=' + VARS]
vm = VM('disk-%s-uefi' % mode, uefi + common)
# (shim's GRUB has no serial terminal built in: no GRUB step here)
if mode == 'crypt':
    try:
        vm.expect('Enter passphrase for', 1800)
    except (TimeoutError, RuntimeError):
        boot_failed(vm, 'uefi', 'UEFI: kernel and initramfs asked for the passphrase')
    vm.send(PASS + '\n')
login(vm)
check(vm, 'started with UEFI', 'ls -d /sys/firmware/efi && echo UEFI', 'UEFI')
check(vm, 'efivarfs mounted', 'grep efivarfs /proc/mounts', 'efivarfs')
check(vm, 'my-secureboot status', 'my-secureboot status', 'installation = shim')
# per-program permissions: my-run hides the camera (/dev/video*) and the home folder
check(vm, 'permissions: sandbox shows /dev without camera',
      'mkdir -p ~/.config/my-distro; echo "ls camera=no microphone=yes home=none" > ~/.config/my-distro/permissions; touch ~/secret.txt; my-run ls /dev | tr "\n" " "; echo; my-run ls ~ | grep -c secret.txt', 'null')
vm.quit()

summary()
