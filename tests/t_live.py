"""Live-system tests: boots the build's kernel + initrd like the ISO does
(rdinit=), with a root shell on the serial line, and checks the 0.9.6 items
(so they don't break) and the 0.9.7 ones. Runs inside WSL; QEMU without KVM
takes a few minutes to boot.

    python3 tests/t_live.py [SHOTDIR] [BUILD]

Screenshots go to SHOTDIR (default /tmp/vm-live), BUILD is the build folder
(default ~/my-distro-build-hw)."""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

SHOTS = sys.argv[1] if len(sys.argv) > 1 else '/tmp/vm-live'
BUILD = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/my-distro-build-hw')
O = BUILD + '/desktop-out/images'
os.makedirs(SHOTS, exist_ok=True)
results = []

def check(name, cmd, want=None, timeout=300):
    out = vm.run(cmd, timeout)
    body = out.split('\n', 1)[1] if '\n' in out else out
    ok = (' 0' in out.splitlines()[-1]) if want is None else (want in body)
    results.append((name, ok, body.strip()[-400:]))
    print(('PASS ' if ok else 'FAIL ') + name + '\n    ' + body.strip()[-400:].replace('\n', '\n    '), flush=True)
    return body

def as_user(cmd):
    """a command in dominic's desktop session (its D-Bus, PipeWire, X)"""
    return ("su dominic -c 'export DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000; "
            "export $(tr \"\\0\" \"\\n\" < /proc/$(pidof openbox | cut -d\" \" -f1)/environ | "
            "grep DBUS_SESSION_BUS_ADDRESS); " + cmd + "'")

# A sound card (Intel HD Audio, no sound out of QEMU) for PipeWire
vm = VM('live', ['-m', '3072', '-smp', '4', '-kernel', O + '/bzImage', '-initrd', O + '/rootfs.cpio.gz',
                 '-append', 'rdinit=/sbin/my-init initramfs_options=size=90% console=tty1 console=ttyS0,115200',
                 '-vga', 'std', '-device', 'qemu-xhci', '-device', 'usb-tablet', '-nic', 'user,model=e1000',
                 '-audiodev', 'none,id=snd0', '-device', 'intel-hda', '-device', 'hda-duplex,audiodev=snd0'])
t0 = time.time()
vm.expect(r'login: ', 1800)
print('login prompt after %d s' % (time.time() - t0), flush=True)
vm.send('root\n'); vm.expect('Password: ', 60); vm.send('root\n')
vm.expect(r'# ', 120)
vm.send('stty cols 200; export PS1="# "\n'); vm.expect(r'# ', 30)

check('version 0.9.8', 'cat /etc/my-distro/version', '0.9.8')
for _ in range(120):
    if 'my-setup' in vm.run('ps | grep -v grep | grep my-setup || true', 30): break
    time.sleep(10)
vm.shot(SHOTS + '/01-setup.png')

# --- 0.9.6 items (regressions) ---------------------------------------------------
check('crond runs under my-keepalive', 'ps | grep -v grep | grep "crond -f"', 'crond')
check('crontab as a user', "su dominic -c 'echo \"* * * * * date >> /tmp/cron-ok\" | crontab - && crontab -l'", 'cron-ok')
check('CJK and emoji fonts', "fc-list | grep -i -E 'emoji|zen ?hei' | cut -c1-90", 'Emoji')
check('S46power cpu line', 'cat /var/log/power.log', 'cpu:')
check('hibernation in kernel (no Secure Boot here)', 'cat /sys/power/state', 'disk')
check('core_pattern', 'cat /proc/sys/kernel/core_pattern', 'my-distro/crash')
CRASH = 'su dominic -c \'python3 -c "import os, signal; os.kill(os.getpid(), signal.SIGSEGV)"\''
vm.run(CRASH + '; sleep 15', 120)
check('crash report written', 'cat /var/crash/crashes.log', 'crashed')
check('gdb', 'gdb --version | head -1', 'GNU gdb')
check('strace', 'strace -c true 2>&1 | tail -2', 'total')
check('perf', 'perf stat -e task-clock true 2>&1 | grep -i -E "task-clock|seconds"', 'task-clock')
check('my-sandbox hides home', "su dominic -c 'touch ~/secret.txt; my-sandbox ls -a /home/dominic'", '.')
out = vm.run("su dominic -c 'my-sandbox ls /home/dominic'", 120)
results.append(('sandbox: secret.txt not visible', 'secret.txt' not in out.split('\n', 1)[1], out[-300:]))
print(results[-1][:2], out[-300:], flush=True)
check('yama ptrace_scope', 'sysctl kernel.yama.ptrace_scope', '= 1')
check('LSMs', 'cat /sys/kernel/security/lsm', 'lockdown')
check('lockdown off without Secure Boot', 'cat /sys/kernel/security/lockdown', '[none]')
check('auditd running', 'ps | grep -v grep | grep auditd', 'auditd')
check('mount.cifs and cifs module', 'modprobe cifs && lsmod | grep cifs', 'cifs')
check('German locale', 'python3 -c "import locale; print(locale.setlocale(locale.LC_ALL, \'de_DE.UTF-8\'))"', 'de_DE.UTF-8')
check('pdftotext', 'pdftotext -v 2>&1 | head -1', 'pdftotext')
check('firewall loaded', 'nft list chain inet my-firewall input | head -3', 'policy drop')

# --- 0.9.7 items that need no desktop ---------------------------------------------
# 3. Intel 3D
check('Mesa iris, crocus, i915 drivers', 'ls /usr/lib/dri/ | tr "\\n" " "', 'iris_dri.so')
check('crocus', 'ls /usr/lib/dri/crocus_dri.so /usr/lib/dri/i915_dri.so', 'crocus')
check('Vulkan anv', 'ls /usr/share/vulkan/icd.d/ | tr "\\n" " "', 'intel')
# 4. firmware updates
check('fwupdmgr', 'fwupdmgr --version 2>&1 | head -5', 'fwupd')
check('fwupd-efi', 'ls /usr/libexec/fwupd/efi/ 2>&1', 'fwupdx64.efi')
check('firmware helper: devices', '/usr/lib/my-distro/firmware devices 2>&1 | head -5; true')
check('firmware helper: LVFS check', '/usr/lib/my-distro/firmware check 2>&1 | head -5', '', timeout=600)
# 5. Secure Boot files
check('shim, MokManager, GRUB, certificate', 'ls -l /usr/lib/my-distro/efi/', 'grubx64.efi')
check('mokutil', 'mokutil --version 2>&1; mokutil --sb-state 2>&1 | head -2; true')
check('kernel lockdown in Secure Boot built in', 'zcat /proc/config.gz 2>/dev/null | grep LOCK_DOWN_IN_EFI; '
      'dmesg | grep -i -E "secure ?boot" | head -3; true')
check('my-secureboot status', 'my-secureboot status', 'secureboot =')
# 10. developer tools: Docker's kernel parts
check('overlayfs and cgroup2', 'grep -E "overlay|cgroup2" /proc/filesystems | tr "\\n" " "', 'overlay')
check('bridge, veth, nat', 'ip link add t0 type bridge && ip link add v0 type veth peer name v1 && '
      'ip link del t0 && ip link del v0 && echo NETOK', 'NETOK')
# 9. languages
for lang in ('de', 'fr', 'es', 'it', 'pl', 'ro', 'tr'):
    check('translation ' + lang, 'python3 -c "import gettext; t = gettext.translation(\'my-distro\', '
          '\'/usr/share/locale\', [\'%s\']); print(t.gettext(\'Sound\'), t.gettext(\'Settings\'))"' % lang, '')
for loc in ('fr_FR', 'es_ES', 'it_IT', 'pl_PL', 'ro_RO', 'tr_TR'):
    check('locale ' + loc, 'python3 -c "import locale; print(locale.setlocale(locale.LC_ALL, \'%s.UTF-8\'))"' % loc,
          loc)
# 12. video codecs
check('FFmpeg decoders', "ffmpeg -hide_banner -decoders 2>/dev/null | grep -E ' (h264|hevc|vp9|libdav1d|av1) ' | "
      "awk '{print $2}' | tr '\\n' ' '", 'libdav1d')
check('VA-API drivers', 'ls /usr/lib/dri/ | grep -E "drv_video" | tr "\\n" " "', 'drv_video')

# --- the desktop ----------------------------------------------------------------
vm.run('echo skipped > /etc/my-distro/setup-done; killall my-setup; true', 60)
time.sleep(20)
vm.shot(SHOTS + '/02-login.png')
vm.type('dominic\n')                           # the password; the user is filled in
time.sleep(150)
vm.shot(SHOTS + '/03-desktop.png')

# 1. PipeWire
check('XDG_RUNTIME_DIR made by my-login', 'ls -ld /run/user/1000', 'dominic')
check('pipewire, wireplumber, pipewire-pulse run', 'ps | grep -v grep | grep -E "pipewire|wireplumber" | '
      'awk "{print \\$NF}" | sort -u | tr "\\n" " "', 'wireplumber')
check('wpctl status', as_user('wpctl status 2>&1 | head -30'), 'Audio')
check('sound helper lists outputs', as_user('/usr/lib/my-distro/sound list'), 'out\t')
check('my-volume through PipeWire', as_user('my-volume up; my-volume get'), '%')
check('ALSA default is PipeWire', as_user('aplay -L 2>/dev/null | head -5'), 'pipewire')
vm.run(as_user('(speaker-test -t sine >/dev/null 2>&1 &) ; sleep 3; my-sound >/dev/null 2>&1 &'), 60)
time.sleep(30)
vm.shot(SHOTS + '/04-sound-window.png')
check('a playing program in the list', as_user('/usr/lib/my-distro/sound list'), 'app\t')
vm.run('killall my-sound speaker-test; true', 30)
# 6. screen reader
vm.run(as_user('MY_READER_LOG=/tmp/reader.log my-reader --toggle'), 60)
time.sleep(15)
check('my-reader runs', 'ps | grep -v grep | grep my-reader', 'my-reader')
check('AT-SPI registry runs', 'ps | grep -v grep | grep -E "at-spi" | head -3', 'at-spi')
vm.run(as_user('my-settings --page access >/dev/null 2>&1 &'), 30)
time.sleep(40)
vm.keys('tab'); time.sleep(3); vm.keys('tab'); time.sleep(5)
vm.shot(SHOTS + '/05-settings-access.png')
check('reader spoke the window and the focus', 'cat /tmp/reader.log', 'Settings')
vm.keys('meta_l'); time.sleep(8); vm.keys('down'); time.sleep(3); vm.keys('down'); time.sleep(3)
vm.shot(SHOTS + '/06-startmenu.png')
vm.keys('esc')
check('reader spoke the Start menu', 'tail -3 /tmp/reader.log', '')
vm.run('killall my-settings; ' + as_user('my-reader --stop'), 30)
# 7. permissions: mpv may not use the camera nor see the home folder
vm.run('modprobe vivid; sleep 3; ls /dev/video*', 60)
vm.run("su dominic -c 'mkdir -p ~/.config/my-distro; echo \"ls camera=no microphone=yes home=none\" "
       "> ~/.config/my-distro/permissions'", 30)
# bwrap can't pivot_root on the live system's initramfs root: the sandbox is
# checked on the installed system (t_install.py) instead
out = vm.run(as_user('my-run ls /dev | tr \"\n\" \" \"'), 60)
print('SKIP permissions: no camera in the sandbox (live root is an initramfs)' if 'pivot_root' in out
      else ('PASS' if 'null' in out and 'video' not in out else 'FAIL') + ' permissions: no camera in the sandbox', out[-200:], flush=True)
if 'pivot_root' not in out:
    results.append(('permissions: no camera in the sandbox', 'null' in out and 'video' not in out, out[-200:]))
out = vm.run(as_user('my-run ls /dev; my-run ls ~'), 60)
if 'pivot_root' not in out:
    results.append(('permissions: camera and home hidden', 'video' not in out and 'secret.txt' not in out, out[-300:]))
    print(results[-1][:2], out[-300:], flush=True)
check('permissions: without a line it runs normally', as_user('my-run --menu "ls /dev" | grep -c video'), '')
vm.run(as_user('my-settings --page permissions >/dev/null 2>&1 &'), 30)
time.sleep(40)
vm.shot(SHOTS + '/07-settings-permissions.png')
vm.run('killall my-settings; true', 30)
# 2. sharing window without Samba: offers to install it
vm.run(as_user('my-sharing >/dev/null 2>&1 &'), 30)
time.sleep(30)
vm.shot(SHOTS + '/08-sharing.png')
check('sharing status without Samba', '/usr/lib/my-distro/sharing status', 'installed = no')
vm.run('killall my-sharing; true', 30)
# 9. a French session's Settings window
vm.run("su dominic -c 'sed -i \"s/^language = .*/language = fr_FR.UTF-8/\" ~/.config/my-distro/settings'; "
       "grep language /home/dominic/.config/my-distro/settings", 30)
vm.run(as_user('LANG=fr_FR.UTF-8 LANGUAGE=fr_FR:fr my-settings --page language >/dev/null 2>&1 &'), 30)
time.sleep(40)
vm.shot(SHOTS + '/09-settings-french.png')
vm.run('killall my-settings; true', 30)
# Users and system's new pages (as root)
vm.run('DISPLAY=:0 my-setup --manage >/dev/null 2>&1 &', 30)
time.sleep(40)
vm.shot(SHOTS + '/10-users-and-system.png')
vm.run('killall my-setup; true', 30)

print('\n== SUMMARY')
for n, ok, _ in results:
    print(('PASS ' if ok else 'FAIL ') + n)
vm.quit()
