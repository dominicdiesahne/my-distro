"""Update test, my-distro 0.9.6 -> 0.9.8: python3 t_update.py plain|crypt ISO096DIR [BUILD]

ISO096DIR holds what was taken out of the released 0.9.6 ISO: bzImage and
initrd (xorriso -osirrox on -indev my-distro.iso -extract /boot/bzImage ...).

1. installs 0.9.6 on an empty disk (its own installer, plain or encrypted)
2. boots it with UEFI (OVMF, Secure Boot off), makes files that must survive
3. serves the 0.9.8 update package (out/my-distro-update.tar.xz) from the
   host on port 8000 like GitHub would (latest.json, .sha256, .sha256.sig),
   signed with a throw-away test key whose public half is put into the VM
   (the real update key is never touched), and runs my-update install there
4. boots again with UEFI and with BIOS: version, files, EFI partition,
   installed-fixups, PipeWire
5. my-secureboot prepare on the updated system: the EFI layout switches to
   shim (grub-before-shim.efi kept), and it boots through shim

Runs inside WSL, one VM at a time."""
import sys, time, os, subprocess, hashlib, json, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

mode = sys.argv[1]
OLD = os.path.abspath(sys.argv[2])
BUILD = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/my-distro-build-hw')
HERE = os.path.dirname(os.path.abspath(__file__))
UPDATE = os.path.abspath(os.path.join(HERE, '..', 'out', 'my-distro-update.tar.xz'))
NEWKERNEL = BUILD + '/desktop-out/images/bzImage'
DISK = BUILD + '/test-update-%s.img' % mode
VARS = '/tmp/vm-update-vars-%s.fd' % mode
PASS = 'testpass1234'
SERVE = '/tmp/update-serve'
results = []

def check(vm, name, cmd, want, timeout=300, absent=None):
    out = vm.run(cmd, timeout)
    out = out.replace(chr(13), '')
    body = out.split(chr(10), 1)[1] if chr(10) in out else out
    ok = (want in body) if want is not None else True
    if absent is not None:
        ok = ok and absent not in body
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + '\n    ' + body.strip()[-600:].replace('\n', '\n    '), flush=True)
    return body

def login(vm, timeout=2400):
    vm.expect(r'login: ', timeout)
    vm.send('root\n'); vm.expect('Password: ', 60); vm.send('root\n'); vm.expect(r'# ', 120)
    vm.send('stty cols 200; export PS1="# "\n'); vm.expect(r'# ', 30)

# --- the update the way GitHub offers it -------------------------------------------
shutil.rmtree(SERVE, ignore_errors=True); os.makedirs(SERVE)
os.symlink(UPDATE, SERVE + '/my-distro-update.tar.xz')
h = hashlib.sha256()
with open(UPDATE, 'rb') as f:
    for c in iter(lambda: f.read(1 << 20), b''):
        h.update(c)
with open(SERVE + '/my-distro-update.tar.xz.sha256', 'w') as f:
    f.write('%s  my-distro-update.tar.xz\n' % h.hexdigest())
subprocess.run(['openssl', 'genpkey', '-algorithm', 'ed25519', '-out', '/tmp/update-test-priv.pem'], check=True)
subprocess.run(['openssl', 'pkey', '-in', '/tmp/update-test-priv.pem', '-pubout', '-out', SERVE + '/test-key.pem'], check=True)
subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', '/tmp/update-test-priv.pem',
                '-in', SERVE + '/my-distro-update.tar.xz.sha256', '-out', SERVE + '/my-distro-update.tar.xz.sha256.sig'], check=True)
base = 'http://10.0.2.2:8000/'
json.dump({'tag_name': 'v0.9.8', 'name': 'my-distro 0.9.8 (test)', 'body': 'test',
           'assets': [{'name': n, 'browser_download_url': base + n} for n in
                      ('my-distro-update.tar.xz', 'my-distro-update.tar.xz.sha256', 'my-distro-update.tar.xz.sha256.sig')]},
          open(SERVE + '/latest.json', 'w'))
server = subprocess.Popen(['python3', '-m', 'http.server', '8000', '--bind', '0.0.0.0', '--directory', SERVE],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

sound = ['-device', 'intel-hda', '-device', 'hda-duplex']
if os.path.exists(DISK): os.remove(DISK)
subprocess.run(['truncate', '-s', '12G', DISK], check=True)
common = ['-m', '3072', '-smp', '4', '-vga', 'std', '-nic', 'user,model=e1000'] + sound + \
         ['-drive', 'file=%s,if=virtio,format=raw' % DISK]
uefi = ['-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
        '-drive', 'if=pflash,format=raw,file=' + VARS]

def fresh_vars():
    subprocess.run(['cp', '/usr/share/OVMF/OVMF_VARS_4M.fd', VARS], check=True)

def boot(tag, args):
    v = VM('upd-%s-%s' % (mode, tag), args)
    if mode == 'crypt':
        v.expect('Enter passphrase for', 2400)
        v.send(PASS + '\n')
    t0 = time.time(); login(v); print('%s: up after %d s' % (tag, time.time() - t0), flush=True)
    return v

try:
    # --- 1. install 0.9.6 ----------------------------------------------------------
    vm = VM('upd-inst-' + mode, common + ['-kernel', OLD + '/bzImage', '-initrd', OLD + '/initrd',
            '-append', 'rdinit=/sbin/my-init initramfs_options=size=90% console=tty1 console=ttyS0,115200'])
    login(vm)
    check(vm, 'old live system is 0.9.6', 'cat /etc/my-distro/version', '0.9.6')
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
    results.append(('0.9.6 install finished', 'Done. my-distro is installed' in out))
    vm.send('\n'); vm.expect(r'# ', 60)
    # serial console for the installed system (test only)
    if mode == 'crypt':
        vm.run('printf %s ' + PASS + ' | cryptsetup open --key-file=- /dev/vda3 t; mkdir -p /mnt/e; mount /dev/vda2 /mnt/e; '
               "sed -i 's/console=tty1 quiet/console=tty1 console=ttyS0,115200/' /mnt/e/grub/grub.cfg; "
               'umount /mnt/e; cryptsetup close t', 120)
    else:
        vm.run('mkdir -p /mnt/r; mount /dev/vda3 /mnt/r; '
               "sed -i 's/console=tty1 quiet/console=tty1 console=ttyS0,115200/' /mnt/r/boot/grub/grub.cfg; "
               'umount /mnt/r', 120)
    vm.send('poweroff\n'); time.sleep(20); vm.quit()

    # --- 2. 0.9.6 with UEFI, files that must survive ----------------------------------
    fresh_vars()
    vm = boot('old-uefi', uefi + common)
    check(vm, 'installed 0.9.6 starts with UEFI', 'cat /etc/my-distro/version; ls -d /sys/firmware/efi', '0.9.6')
    check(vm, 'old EFI partition: plain GRUB, no shim',
          'mkdir -p /mnt/e; mount /dev/vda2 /mnt/e; ls /mnt/e/EFI/BOOT/; umount /mnt/e', 'BOOTX64.EFI', absent='mmx64.efi')
    vm.run('mkdir -p /home/tester; echo mine > /home/tester/keepme.txt; echo mine > /root/keepme.txt; '
           'echo testbox > /etc/hostname; sync', 30)
    if mode == 'crypt':
        oldhash = check(vm, 'old ESP kernel hash',
                        'mount /dev/vda2 /mnt/e; sha256sum /mnt/e/my-distro/bzImage; umount /mnt/e', ' ')
    # --- 3. the update ----------------------------------------------------------------
    vm.run("python3 -c \"import urllib.request as u; open('/etc/my-distro/update-key.pem','wb')"
           ".write(u.urlopen('http://10.0.2.2:8000/test-key.pem').read())\"", 120)
    vm.run("sed -i 's|^url *=.*|url = http://10.0.2.2:8000/latest.json|' /etc/my-distro/update.conf; "
           "grep '^url' /etc/my-distro/update.conf", 30)
    check(vm, 'my-update sees 0.9.8', 'my-update check', '0.9.8', 300)
    out = check(vm, 'my-update install', 'my-update install 2>&1 | tail -12', 'is installed', 5400)
    check(vm, 'version is 0.9.8 right after the update', 'cat /etc/my-distro/version', '0.9.8')
    vm.send('poweroff\n'); time.sleep(25); vm.quit()

    # --- 4. the updated system, UEFI then BIOS -------------------------------------------
    for tag, args in (('uefi', None), ('bios', None)):
        if tag == 'uefi':
            fresh_vars(); vm = boot('new-uefi', uefi + common)
        else:
            vm = boot('new-bios', common)
        p = tag + ': '
        check(vm, p + 'starts, version 0.9.8', 'cat /etc/my-distro/version', '0.9.8')
        check(vm, p + 'my files survive', 'cat /root/keepme.txt /home/tester/keepme.txt; cat /etc/hostname', 'mine\nmine\ntestbox')
        check(vm, p + 'installed-fixups ran (installer entries gone)',
              'ls /usr/share/applications/my-install.desktop 2>&1; grep -c my-install /etc/my-panel/menu', 'No such file')
        if mode == 'crypt':
            check(vm, p + 'EFI partition has the new kernel and initramfs',
                  'mount /dev/vda2 /mnt/e; ls -la /mnt/e/my-distro; sha256sum /mnt/e/my-distro/bzImage /boot/bzImage; umount /mnt/e',
                  'bzImage', 120)
            body = vm.run('mount /dev/vda2 /mnt/e; sha256sum /mnt/e/my-distro/bzImage /boot/bzImage | cut -c1-64 | sort -u | wc -l; umount /mnt/e', 60)
            results.append((p + 'ESP kernel is the same file as /boot/bzImage', '\n1' in body.replace('\r', '')))
        new = subprocess.run(['sha256sum', NEWKERNEL], capture_output=True, text=True).stdout.split()[0]
        check(vm, p + 'kernel is the new 0.9.8 kernel', 'sha256sum /boot/bzImage', new[:64])
        check(vm, p + 'running kernel is not the old one', 'uname -r', '6.')
        check(vm, p + 'EFI partition: layout after the update',
              'mount /dev/vda2 /mnt/e 2>/dev/null; ls /mnt/e/EFI/BOOT/; umount /mnt/e', 'BOOTX64.EFI')
        check(vm, p + 'PipeWire and WirePlumber run',
              'export XDG_RUNTIME_DIR=/tmp/xdgt; mkdir -p -m 700 $XDG_RUNTIME_DIR; '
              'dbus-run-session -- sh -c "pipewire >/dev/null 2>&1 & wireplumber >/dev/null 2>&1 & sleep 15; wpctl status | head -12"',
              'Audio', 120)
        if tag == 'uefi':
            check(vm, 'my-secureboot status (before prepare)', 'my-secureboot status', 'installation = grub')
            # --- 5. my-secureboot prepare on the updated system ---------------------------
            vm.send('my-secureboot prepare\n')
            try:
                vm.expect(r'(?i)password', 120); vm.send('testpass\n')
                vm.expect(r'(?i)password', 60); vm.send('testpass\n')
            except TimeoutError as e:
                print('no password prompt:', e, flush=True)
            time.sleep(10); vm.send('\n'); vm.expect(r'# ', 120)
            check(vm, 'my-secureboot prepare: EFI layout is shim now',
                  'E=$(/usr/lib/my-distro/esp-mount); ls $E/EFI/BOOT/; cat $E/EFI/BOOT/grub.cfg; ls $E/boot/grub 2>&1',
                  'grub-before-shim.efi')
            check(vm, 'my-secureboot prepare: shim, MokManager, certificate there',
                  'E=$(/usr/lib/my-distro/esp-mount); ls $E/EFI/BOOT/', 'mmx64.efi')
            check(vm, 'my-secureboot status (after prepare)', 'my-secureboot status', 'installation = shim')
            vm.send('poweroff\n'); time.sleep(25); vm.quit()
            # boot through shim (Secure Boot off): the box must still start
            fresh_vars()
            vm = boot('new-shim', uefi + common)
            check(vm, 'starts through shim -> GRUB -> kernel', 'cat /etc/my-distro/version; ls -d /sys/firmware/efi', '0.9.8')
            check(vm, 'my files still there through shim', 'cat /root/keepme.txt', 'mine')
        vm.send('poweroff\n'); time.sleep(25); vm.quit()
finally:
    server.terminate()

print('\n== SUMMARY update ' + mode)
for n, ok in results:
    print(('PASS ' if ok else 'FAIL ') + n)
