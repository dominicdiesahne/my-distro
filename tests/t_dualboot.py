"""Install next to another system (1.4.0). python3 t_dualboot.py ISO [SHOTDIR]

1. boots the ISO with UEFI (OVMF, Secure Boot off) and a 16 GB disk that
   looks like a Windows PC's: GPT, a 100 MB EFI partition with
   EFI/Microsoft/Boot/bootmgfw.efi, Microsoft's reserved partition, an NTFS
   "C:" with a file on it, and 12 GB free (as after Shrink Volume)
2. runs my-install on the serial line and picks "next to it"
3. checks that nothing of "Windows" changed (its partitions, their
   contents, its start files), boots the disk with the same firmware
   memory: the firmware's own boot list starts my-distro (efibootmgr's
   entry), whose boot menu also has an entry for Windows

The live system has no shell on the serial line: the test logs in on the
console (like t_secureboot.py) and starts one there.
Runs inside WSL; needs OVMF (Kali/Debian package ovmf)."""
import sys, time, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

ISO = os.path.abspath(sys.argv[1])
SHOTS = sys.argv[2] if len(sys.argv) > 2 else '/tmp/vm-dualboot'
os.makedirs(SHOTS, exist_ok=True)
DISK = '/tmp/vm-dualboot-disk.img'
VARS = '/tmp/vm-dualboot-vars.fd'
results = []

def check(name, cmd, want, timeout=300):
    out = vm.run(cmd, timeout)
    ok = want in out
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + '\n    ' + out.strip()[-600:].replace('\n', '\n    '), flush=True)
    return out

def summary():
    print('\n== SUMMARY dualboot')
    for n, ok in results:
        print(('PASS ' if ok else 'FAIL ') + n)

if os.path.exists(DISK):
    os.remove(DISK)
subprocess.run(['truncate', '-s', '16G', DISK], check=True)
subprocess.run(['cp', '/usr/share/OVMF/OVMF_VARS_4M.fd', VARS], check=True)
fw = ['-machine', 'q35', '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
      '-drive', 'if=pflash,format=raw,unit=1,file=' + VARS,
      '-m', '3072', '-smp', '2', '-vga', 'std', '-nic', 'user,model=e1000',
      '-drive', 'file=%s,if=virtio,format=raw' % DISK]

# --- 1. the live system with UEFI, a shell on the serial line -----------------
vm = VM('dualboot', fw + ['-cdrom', ISO, '-boot', 'd'])
time.sleep(240)                                  # GRUB's 5 s, the kernel, the desktop's login screen
vm.shot(SHOTS + '/01-live.png')
vm.keys('ctrl-alt-f1'); time.sleep(5)
up = False
for i in range(5):
    for _ in range(2):
        vm.keys('ctrl-u'); time.sleep(0.5)
        vm.keys('ctrl-d'); time.sleep(5)
    vm.type('root\n'); time.sleep(5); vm.type('root\n'); time.sleep(8)
    # a root shell on the serial line, with it as its terminal (my-install
    # reads from /dev/tty)
    vm.type("setsid sh -c 'exec sh -i <>/dev/ttyS0 >&0 2>&1' &\n"); time.sleep(5)
    vm.send('\necho SERIAL-SHELL-$((40+2))\n')
    try:
        vm.expect('SERIAL-SHELL-42', 60)
        up = True
        break
    except TimeoutError:
        vm.shot(SHOTS + '/02-login-%d.png' % i)
        time.sleep(10)
if not up:
    results.append(('a shell on the serial line', False))
    summary(); vm.quit(); sys.exit(1)
vm.send('stty -echo cols 200; export PS1="# "\n'); time.sleep(2)
check('live system started with UEFI', 'ls -d /sys/firmware/efi && echo UEFI', 'UEFI')

# --- 2. the disk of a "Windows PC" ----------------------------------------------
vm.run('sfdisk /dev/vda <<EOF\nlabel: gpt\n'
       'size=100MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI system partition"\n'
       'size=16MiB, type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE, name="Microsoft reserved partition"\n'
       'size=3GiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="Basic data partition"\nEOF\n'
       'sleep 3; mkfs.fat -F 32 -n SYSTEM /dev/vda1; mkntfs -Q -L Windows /dev/vda3', 300)
vm.run('mkdir -p /mnt/e /mnt/w; mount /dev/vda1 /mnt/e; mkdir -p /mnt/e/EFI/Microsoft/Boot /mnt/e/EFI/BOOT; '
       'head -c 1500000 /dev/urandom > /mnt/e/EFI/Microsoft/Boot/bootmgfw.efi; '
       'cp /mnt/e/EFI/Microsoft/Boot/bootmgfw.efi /mnt/e/EFI/BOOT/BOOTX64.EFI; '
       'ntfs-3g /dev/vda3 /mnt/w; mkdir -p /mnt/w/Users/anna; head -c 5000000 /dev/urandom > /mnt/w/Users/anna/photo.jpg; '
       'sync; umount /mnt/w /mnt/e', 300)
before = check('the "Windows" disk', 'sfdisk -d /dev/vda | grep "^/dev"; '
               'echo SUMS; sha256sum /dev/vda1 /dev/vda2 /dev/vda3 | cut -c1-16', 'SUMS', 600)
sums = before.split('SUMS')[-1].split()[:3]

# --- 3. my-install, next to it ----------------------------------------------------
vm.send('/usr/sbin/my-install\n')
vm.expect(r'Number \(1-\d\): ', 300); vm.send('1\n')
out = vm.expect(r'Which\? Number \(1-2\): |Type 1 to continue: ', 120)
print(out[-1500:], flush=True)
ok = 'Which? Number (1-2)' in out
results.append(('the installer offers "next to it" (12 GB free)', ok))
if not ok:
    vm.send('no\n'); summary(); vm.quit(); sys.exit(1)
vm.send('2\n')
vm.expect(r'Type yes or no \[no\]: ', 60); vm.send('no\n')
vm.expect('Type yes to continue: ', 60); vm.send('yes\n')
out = vm.expect(r'(Press Enter to close this window\.)', 3600)
print(out[-2000:], flush=True)
results.append(('install finished', 'Done. my-distro is installed' in out))
results.append(('the boot menu got Windows', 'Boot menu: Windows' in out))
vm.send('\n'); time.sleep(3)
print(vm.run('tail -25 /tmp/my-install.log', 60), flush=True)
after = check('"Windows" is unchanged: partitions 1-3 as before, a 4th for my-distro',
              'sfdisk -d /dev/vda | grep "^/dev"; echo SUMS; sha256sum /dev/vda2 /dev/vda3 | cut -c1-16', 'vda4', 600)
sums2 = after.split('SUMS')[-1].split()[:2]
results.append(('... the contents of its reserved and NTFS partitions too', sums2 == sums[1:3]))
print('before', sums, 'after', sums2, flush=True)
check('... its start files too, and my-distro\'s are in EFI/my-distro',
      'mount /dev/vda1 /mnt/e; cmp /mnt/e/EFI/Microsoft/Boot/bootmgfw.efi /mnt/e/EFI/BOOT/BOOTX64.EFI && echo WINDOWS-INTACT; '
      'ls /mnt/e/EFI/my-distro | tr "\\n" " "', 'WINDOWS-INTACT')
check('... and its NTFS files', 'ntfs-3g -o ro /dev/vda3 /mnt/w && ls -l /mnt/w/Users/anna; umount /mnt/w', 'photo.jpg')
check('a firmware boot entry for my-distro, first', 'efibootmgr', 'my-distro')
# (the installed system's console on the serial line too, test only)
vm.run('umount /mnt/e; mkdir -p /mnt/r; mount /dev/vda4 /mnt/r; '
       "sed -i 's/console=tty1 quiet/console=tty1 console=ttyS0,115200/' /mnt/r/boot/grub/grub.cfg; "
       'grep -A7 \'menuentry "Windows"\' /mnt/r/boot/grub/grub.cfg; umount /mnt/r; sync', 120)
vm.send('poweroff -f\n'); time.sleep(10)
vm.quit()

# --- 4. the disk alone, the same firmware memory ------------------------------
vm = VM('dualboot-disk', fw)
try:
    vm.expect(r'login: ', 2400)
    vm.send('root\n'); vm.expect('Password: ', 60); vm.send('root\n'); vm.expect(r'# ', 120)
    vm.send('stty cols 200; export PS1="# "\n'); vm.expect(r'# ', 30)
    results.append(('the installed my-distro starts', True))
    check('it started through its own firmware entry', 'efibootmgr | grep -E "BootCurrent|my-distro"; '
          'cur=$(efibootmgr | sed -n "s/BootCurrent: //p"); efibootmgr | grep "^Boot$cur"', 'my-distro')
    check('its boot menu also starts Windows', '/usr/lib/my-distro/esp-mount >/dev/null; '
          'grep -A7 \'menuentry "Windows"\' /boot/grub/grub.cfg', 'chainloader /EFI/Microsoft/Boot/bootmgfw.efi')
    check('my-secureboot finds its files in EFI/my-distro', 'my-secureboot status', 'installation = shim')
except TimeoutError as e:
    print('no login:', str(e)[-800:], flush=True)
    vm.shot(SHOTS + '/09-disk-boot.png')
    results.append(('the installed my-distro starts', False))
vm.quit()
summary()
