"""Live-system tests: boots the build's kernel + initrd like the ISO does
(rdinit=), with a root shell on the serial line, and checks the 0.9.6 items
(so they don't break) and the 0.9.7 ones. Runs inside WSL; QEMU without KVM
takes a few minutes to boot.

    python3 tests/t_live.py [SHOTDIR] [BUILD]

Screenshots go to SHOTDIR (default /tmp/vm-live), BUILD is the build folder
(default ~/my-distro-build-hw)."""
import sys, time, os, re, base64
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

SHOTS = sys.argv[1] if len(sys.argv) > 1 else '/tmp/vm-live'
BUILD = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/my-distro-build-hw')
O = BUILD + '/desktop-out/images'
os.makedirs(SHOTS, exist_ok=True)
results = []

def check(name, cmd, want=None, timeout=300):
    out = vm.run(cmd, timeout)
    body = VM.body(out)
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

check('version 1.3.0', 'cat /etc/my-distro/version', '1.3.0')
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
# (0.9.9: bwrap runs on the live system too; these used to pass even when
# it failed with "pivot_root: Invalid argument", so they want its output)
# (rootfs is its own parent in mountinfo; init's bind mount has rootfs above it)
check('live root is a mount (bwrap can pivot_root)',
      'awk \'$5 == "/" { print ($1 == $2 ? "rootfs" : "mount" "ed") }\' /proc/self/mountinfo', 'mounted')
check('my-sandbox hides home', "su dominic -c 'touch ~/secret.txt; my-sandbox ls -a /home/dominic && echo SANDBOX-RAN-$((2+3))'",
      'SANDBOX-RAN-5')  # not in the echoed command line, only in the output
out = vm.run("su dominic -c 'my-sandbox ls -a /home/dominic; echo RC=$?'", 120)
body = out.split('\n', 1)[1]
results.append(('sandbox: secret.txt not visible', 'secret.txt' not in body and 'RC=0' in body, out[-300:]))
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
# in libva's driverdir: Intel's i965 and Mesa's (radeonsi, ...; since 1.1 with libglvnd)
check('VA-API drivers in libva\'s folder', 'ls /usr/lib/va/ | grep -E "drv_video" | tr "\\n" " "', 'radeonsi_drv_video')
check('VA-API: none left in /usr/lib/dri', 'echo DRI=$(ls /usr/lib/dri/ | grep -c drv_video)', 'DRI=0')

# --- 1.0.0 items that need no desktop or hardware ---------------------------------
# OpenVPN next to WireGuard
check('openvpn', 'openvpn --version | head -1', 'OpenVPN')
check('tun device can be made', 'modprobe tun; ip tuntap add dev tt0 mode tun && ip link del tt0 && echo TUNOK', 'TUNOK')
check('FFmpeg: ALSA input and AAC encoder', "ffmpeg -hide_banner -devices 2>&1 | grep -w alsa; "
      "ffmpeg -hide_banner -encoders 2>&1 | grep -w aac", 'aac')
OVPN = ('client\\ndev tun\\nremote vpn.example.com 1194\\nauth-user-pass\\nredirect-gateway\\n'
        'up /bin/evil\\nscript-security 2\\n<ca>\\n-----BEGIN CERTIFICATE-----\\nMIIB\\n-----END CERTIFICATE-----\\n</ca>\\n')
check('my-vpnctl imports an .ovpn (redirect-gateway gets def1)',
      "printf '" + OVPN + "' > /tmp/t.ovpn; su dominic -c 'my-vpnctl import-ovpn tt < /tmp/t.ovpn'; "
      "cat /etc/my-distro/vpn/tt.ovpn", 'redirect-gateway def1')
check('.ovpn: up and script-security are not kept',
      'grep -c -E "^(up|script-security)" /etc/my-distro/vpn/tt.ovpn; true', '0')
check('my-vpnctl: OpenVPN login is root-only', "printf 'me\\nsecret\\n' | su dominic -c 'my-vpnctl auth tt'; "
      "ls -l /etc/my-distro/vpn/tt.auth", '-rw-------')
check('my-vpnctl lists it as ovpn', "su dominic -c 'my-vpnctl list' | tr '\\t' ' '", 'tt ovpn')
check('my-vpnctl refuses a file reference',
      "printf 'client\\nremote a 1\\nca /etc/shadow\\n' | su dominic -c 'my-vpnctl import-ovpn bad'; "
      "ls /etc/my-distro/vpn/bad.ovpn 2>&1", 'No such file')
# 1.2.0: a private key with a password (my-vpnctl keypass -> openvpn --askpass).
# No server: with the right password openvpn gets as far as sending to it.
vm.run('cd /tmp && openssl req -x509 -newkey rsa:2048 -keyout k.pem -passout pass:right -out c.pem '
       '-subj /CN=t -days 2 2>/dev/null; { printf "client\\ndev tun\\nproto udp\\nremote 127.0.0.1 1194\\n'
       '<ca>\\n"; cat c.pem; printf "</ca>\\n<cert>\\n"; cat c.pem; printf "</cert>\\n<key>\\n"; cat k.pem; '
       'printf "</key>\\n"; } > tk.ovpn; cd /', 120)
check('my-vpnctl imports an encrypted private key',
      "su dominic -c 'my-vpnctl import-ovpn tk < /tmp/tk.ovpn' && echo ENC=$(grep -c ENCRYPTED /etc/my-distro/vpn/tk.ovpn)",
      'ENC=2')
check('my-vpnctl: key password is root-only', "printf 'wrong\\n' | su dominic -c 'my-vpnctl keypass tk'; "
      "ls -l /etc/my-distro/vpn/tk.pass", '-rw-------')
check('vpn: a wrong key password is reported', '/usr/lib/my-distro/vpn up tk', 'password of the private key',
      timeout=120)
vm.run("printf 'right\\n' | su dominic -c 'my-vpnctl keypass tk'", 30)
vm.run('/usr/lib/my-distro/vpn up tk; true', 120)           # no server: it gives up after 45 s
check('vpn: the right key password opens the key',
      'echo PW=$(grep -c -i -E "private key password|load private key" /run/my-vpn.last.log)', 'PW=0')
check('vpn: ... and openvpn tries the server', 'grep -o "link remote" /run/my-vpn.last.log | head -1',
      'link remote')
check('my-vpnctl remove takes the key password too',
      "su dominic -c 'my-vpnctl remove tk'; ls /etc/my-distro/vpn/tk.pass 2>&1", 'No such file')
check('vpn status: disconnected', "su dominic -c 'my-vpnctl status'", 'disconnected')
# VPN reconnect after a network change: nothing to do without a VPN
check('vpn reconnect without a VPN does nothing', '/usr/lib/my-distro/vpn reconnect; echo RC=$?', 'RC=0')
check('DHCP hook runs on a new lease', 'sh /usr/share/udhcpc/default.script.d/my-vpn bound; echo RC=$?', 'RC=0')
check('my-vpnctl removes it', "su dominic -c 'my-vpnctl remove tt'; ls /etc/my-distro/vpn/ | grep -c '^tt'; true", '0')
# 1.3.0: .p12 files. The VPN window runs "openssl pkcs12" with the password on
# its stdin (tried on the build machine with real files); here: the image's
# openssl, and old files with -legacy.
vm.run('mkdir -p /tmp/vt && cd /tmp/vt && '
       'openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -subj /CN=vtca -days 2 2>/dev/null && '
       'for n in s c; do openssl req -newkey rsa:2048 -nodes -keyout $n.key -out $n.csr -subj /CN=$n 2>/dev/null && '
       'openssl x509 -req -in $n.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out $n.crt -days 2 2>/dev/null; done; '
       'openssl pkcs12 -export -legacy -in c.crt -inkey c.key -certfile ca.crt -out old.p12 -passout pass:pw; '
       'openssl pkcs12 -export -in c.crt -inkey c.key -certfile ca.crt -out new.p12 -passout pass:pw; ls; cd /', 300)
check('openssl reads a .p12 (password on stdin)',
      'echo pw | openssl pkcs12 -in /tmp/vt/new.p12 -passin stdin -nocerts -nodes 2>/dev/null | grep -c "BEGIN PRIVATE KEY"',
      '1')
check('openssl reads an old .p12 with -legacy',
      'echo pw | openssl pkcs12 -in /tmp/vt/old.p12 -passin stdin -nocerts -nodes -legacy 2>/dev/null | '
      'grep -c "BEGIN PRIVATE KEY"', '1')
check('openssl refuses a wrong .p12 password',
      'echo no | openssl pkcs12 -in /tmp/vt/new.p12 -passin stdin -nocerts -nodes >/dev/null 2>&1; echo RC=$?', 'RC=1')
# 1.3.0: tap VPNs, against an OpenVPN server in this VM: one that pushes the
# address, one that leaves it to DHCP (a few lines of Python answer on its side)
DHCPD = base64.b64encode(b'''import socket, struct, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
s.setsockopt(socket.SOL_SOCKET, 25, sys.argv[1].encode() + b"\\0")
s.bind(("0.0.0.0", 67))
ip, me = socket.inet_aton("10.9.0.50"), socket.inet_aton("10.9.0.1")
while True:
    d, a = s.recvfrom(2048)
    if len(d) < 240 or d[0] != 1:
        continue
    o, i, mt = d[240:], 0, 0
    while i < len(o) and o[i] != 255:
        if o[i] == 0:
            i += 1
            continue
        if o[i] == 53:
            mt = o[i + 2]
        i += 2 + o[i + 1]
    if mt not in (1, 3):
        continue
    rt = 2 if mt == 1 else 5
    p = bytes([2, 1, 6, 0]) + d[4:8] + b"\\0\\0\\x80\\0" + b"\\0" * 4 + ip + me + b"\\0" * 4 + d[28:44]
    p += b"\\0" * 192 + b"\\x63\\x82\\x53\\x63" + bytes([53, 1, rt, 54, 4]) + me + bytes([51, 4])
    p += struct.pack("!I", 3600) + bytes([1, 4, 255, 255, 255, 0, 3, 4]) + me
    p += bytes([6, 4]) + socket.inet_aton("10.9.0.53") + bytes([255])
    s.sendto(p, ("255.255.255.255", 68))
    print("sent", rt, flush=True)
''').decode()
# (in pieces: the serial console's line buffer is about 4 KB)
vm.run('rm -f /tmp/vt/dhcpd.b64', 30)
for i in range(0, len(DHCPD), 600):
    vm.run('printf %s ' + DHCPD[i:i + 600] + ' >> /tmp/vt/dhcpd.b64', 30)
vm.run('base64 -d /tmp/vt/dhcpd.b64 > /tmp/vt/dhcpd.py; wc -l /tmp/vt/dhcpd.py', 60)
SRV = ('mode server\\ntls-server\\ndev tap9\\nproto udp4\\nlocal 127.0.0.1\\nport 11940\\n'
       'ca /tmp/vt/ca.crt\\ncert /tmp/vt/s.crt\\nkey /tmp/vt/s.key\\ndh none\\nkeepalive 5 30\\nverb 3\\n')

def vpn_server(extra):
    vm.run("kill $(cat /tmp/vt/srv.pid 2>/dev/null) 2>/dev/null; sleep 2; printf '" + SRV + extra + "' > /tmp/vt/srv.conf; "
           'openvpn --config /tmp/vt/srv.conf --daemon vt-server --writepid /tmp/vt/srv.pid --log /tmp/vt/srv.log; '
           'sleep 10; ip address add 10.9.0.1/24 dev tap9; ip link set tap9 up; ip -4 -o address show tap9', 90)

vm.run('cd /tmp/vt && { printf "client\\ndev tap\\nproto udp4\\nremote 127.0.0.1 11940\\nnobind\\n<ca>\\n"; cat ca.crt; '
       'printf "</ca>\\n<cert>\\n"; cat c.crt; printf "</cert>\\n<key>\\n"; cat c.key; printf "</key>\\n"; } > tap.ovpn; cd /',
       30)
check('my-vpnctl imports a tap VPN',
      "su dominic -c 'my-vpnctl import-ovpn tp < /tmp/vt/tap.ovpn'; grep -c '^dev tap' /etc/my-distro/vpn/tp.ovpn", '1')
vpn_server('server-bridge 10.9.0.1 255.255.255.0 10.9.0.10 10.9.0.20\\n')
check('vpn: a tap VPN connects (address pushed)', '/usr/lib/my-distro/vpn up tp; tail -3 /tmp/vt/srv.log',
      'Connected to tp', timeout=240)
check('vpn: ovpn0 is a tap device', 'ip -d link show ovpn0 | grep -o "tun type tap"', 'tun type tap')
check('vpn: the pushed address', 'ip -4 -o address show ovpn0', 'inet 10.9.0.1')
vm.run('/usr/lib/my-distro/vpn down', 60)
vpn_server('server-bridge\\n')                       # no address: the clients use DHCP
# (the server's side is in this VM too, behind this computer's firewall,
# which lets no DHCP requests in: open it for tap9, as on a real server.
# The client's udhcpc needs nothing: it reads the answers before the firewall)
vm.run('nft insert rule inet my-firewall input iifname tap9 accept; '
       '(python3 /tmp/vt/dhcpd.py tap9 >/tmp/vt/dhcpd.log 2>&1 &); sleep 3', 30)
check('vpn: a tap VPN connects (no address pushed)', '/usr/lib/my-distro/vpn up tp', 'Connected to tp', timeout=240)
time.sleep(30)
check('vpn: tap address by DHCP over the VPN', 'ip -4 -o address show ovpn0; tail -3 /tmp/vt/dhcpd.log', 'inet 10.9.0.50')
check('vpn: the lease\'s DNS server is used', 'cat /etc/resolv.conf', '10.9.0.53')
check('vpn: no default route through the tap VPN', 'echo DEF=$(ip route show default | grep -c ovpn0)', 'DEF=0')
vm.run('/usr/lib/my-distro/vpn down', 60)
check('vpn down: udhcpc stopped, DNS back',
      'echo UD=$(ps | grep -v grep | grep -c "udhcpc -i ovpn0") NS=$(grep -c 10.9.0.53 /etc/resolv.conf)', 'UD=0 NS=0')
vm.run("kill $(cat /tmp/vt/srv.pid); pkill -f dhcpd.py; su dominic -c 'my-vpnctl remove tp'; true", 30)
# Wi-Fi with certificates (EAP-TLS): the files go to a root-only place
check('my-wificert saves a key root-only', "echo KEYDATA | su dominic -c 'my-wificert save 6162 key'; "
      "ls -l /etc/my-distro/wifi-certs/", '-rw-------')
check('my-wificert refuses a bad kind', "echo x | su dominic -c 'my-wificert save 6162 ../../x'; echo RC=$?", 'RC=1')
check('my-wificert removes them', "su dominic -c 'my-wificert remove 6162'; "
      "ls /etc/my-distro/wifi-certs/ | grep -c 6162; true", '0')
# wpa_supplicant takes the network block the Wi-Fi window writes for EAP-TLS
vm.run("printf '%s\\n' 'ctrl_interface=DIR=/run/wpa_supplicant' 'network={' ' ssid=\"corp\"' "
       "' key_mgmt=WPA-EAP WPA-EAP-SHA256' ' ieee80211w=1' ' eap=TLS' ' identity=\"me@corp.example\"' "
       "' ca_cert=\"/etc/my-distro/wifi-certs/636f7270-ca\"' "
       "' client_cert=\"/etc/my-distro/wifi-certs/636f7270-cert\"' "
       "' private_key=\"/etc/my-distro/wifi-certs/636f7270-key\"' ' private_key_passwd=\"pw\"' "
       "' domain_suffix_match=\"corp.example\"' '}' > /tmp/tls.conf", 30)
check('wpa_supplicant reads an EAP-TLS network block',
      'out=$(timeout 8 wpa_supplicant -dd -c /tmp/tls.conf -i eth0 -D wired 2>&1); '
      'echo "$out" | grep -q "Priority group" && ! echo "$out" | grep -q -E "failed to parse|Failed to read" '
      '&& echo PARSE-OK', 'PARSE-OK', timeout=60)

# --- the desktop ----------------------------------------------------------------
# 1.3.0: the login screen speaks for the screen reader while this file exists
vm.run('mkdir -p /var/lib/my-distro; touch /var/lib/my-distro/login-reader; '
       'echo skipped > /etc/my-distro/setup-done; killall my-setup; true', 60)
time.sleep(20)
vm.shot(SHOTS + '/02-login.png')
vm.keys('shift-tab'); time.sleep(3)            # to the user name: "User, dominic"
vm.keys('tab'); time.sleep(3)                  # back to the password
vm.type('dominic\n')                           # the password; the user is filled in
time.sleep(150)
vm.shot(SHOTS + '/03-desktop.png')
out = vm.run('cat /run/my-login/speech.log; echo END', 30)
body = VM.body(out)
said = [l.strip() for l in body.splitlines() if l.strip() and not l.startswith('END')]
results.append(('login screen speaks its fields (never the password)',
                any('Log in' in l for l in said) and any('User, dominic' in l for l in said) and
                not any(len(l) == 1 for l in said), body[-300:]))
print(('PASS ' if results[-1][1] else 'FAIL ') + results[-1][0] + '\n    ' + body[-300:], flush=True)
vm.run('rm -f /var/lib/my-distro/login-reader', 30)

# 0. OpenGL through libglvnd (1.1): libGL/libEGL hand the calls to Mesa
check('libglvnd and Mesa\'s vendor libraries', 'test -e /usr/lib/libGLdispatch.so.0 && test -e /usr/lib/libGLX_mesa.so.0 '
      '&& test -e /usr/lib/libEGL_mesa.so.0 && echo GLVND""OK', 'GLVNDOK')
check('OpenGL on the desktop (GLX)', as_user('glxinfo -B 2>&1 | grep -E "OpenGL (vendor|renderer|version)|Error"'),
      'vendor string: Mesa')
check('EGL', as_user('eglinfo -B 2>&1 | grep -m3 -E "EGL vendor|renderer|Error"'), 'Mesa')

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
# 1.0.0: a screen recording has the sound that plays (QEMU's sound card is a
# null device, but PipeWire still runs its monitor, so there is an audio stream)
vm.run(as_user('(speaker-test -t sine >/dev/null 2>&1 &); sleep 2; my-record'), 60)
time.sleep(8)
vm.run(as_user('my-record'), 60)           # stops it
time.sleep(4)
vm.run('killall speaker-test; true', 30)
check('my-record file has video and AAC audio',
      as_user('f=$(ls -t ~/Videos/*.mp4 | head -1); ffmpeg -hide_banner -i "$f" 2>&1 | grep -E "Stream"'),
      'Audio: aac')
check('my-record log shows the sound input', 'grep -c "Input #1, alsa" /tmp/my-record-1000.log; true', '1')
# 1.2.0: the microphone, alone and mixed with the PC sound. QEMU has no
# microphone that sends anything: a virtual one (a null source) stands in
# and becomes the default.
vm.run(as_user('pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=test-mic '
               'node.description=test-mic media.class=Audio/Source/Virtual object.linger=true '
               'audio.position=[FL FR] }" >/dev/null; sleep 2; '
               'wpctl set-default $(wpctl status | sed -n "s/^[^0-9]*\\([0-9][0-9]*\\)\\. test-mic.*/\\1/p" | head -n 1)'),
       60)
check('the virtual microphone is the default', as_user('wpctl inspect @DEFAULT_AUDIO_SOURCE@ | grep node.name'),
      'test-mic')
vm.run(as_user('my-record --sound mic'), 60)
time.sleep(6)
vm.run(as_user('my-record'), 60)
time.sleep(4)
check('my-record --sound mic: video with AAC audio',
      as_user('f=$(ls -t ~/Videos/*.mp4 | head -1); ffmpeg -hide_banner -i "$f" 2>&1 | grep -E "Stream"'),
      'Audio: aac')
vm.run(as_user('(speaker-test -t sine >/dev/null 2>&1 &); sleep 2; my-record --sound both'), 60)
time.sleep(5)
# while it records: one stream on the microphone, one on the output's monitor
check('my-record --sound both: the microphone is linked',
      as_user('pw-link -l | grep -A1 "^test-mic:capture_FL"'), '|->')
check('my-record --sound both: the PC sound (monitor) is linked',
      as_user('pw-link -l | grep -A1 ":monitor_FL"'), '|->')
time.sleep(3)
vm.run(as_user('my-record'), 60)
time.sleep(4)
vm.run('killall speaker-test; true', 30)
check('my-record --sound both: two sound inputs',
      'echo IN=$(grep -c -E "Input #[12], alsa" /tmp/my-record-1000.log)', 'IN=2')
check('my-record --sound both: the microphone by its serial',
      'grep -o "pipewire:NODE=[0-9]*" /tmp/my-record-1000.log | head -1', 'pipewire:NODE=')
check('my-record --sound both: one mixed AAC track',
      as_user('f=$(ls -t ~/Videos/*.mp4 | head -1); echo TRACKS=$(ffmpeg -hide_banner -i "$f" 2>&1 | grep -c "Audio: aac")'),
      'TRACKS=1')
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
vm.run('killall my-settings; true', 30)
# 1.3.0: the lock screen speaks while the reader is on
vm.run(as_user('MY_READER_LOG=/tmp/lock.log my-lock --background'), 60)
time.sleep(10)
vm.type('wrong\n'); time.sleep(10)
vm.shot(SHOTS + '/05b-lock.png')
for _ in range(3):                             # unlock (the next tests need the keyboard)
    vm.type('dominic\n'); time.sleep(10)
    if 'LOCKED' not in vm.run('ps | grep -v grep | grep -q "my-lock --background" && echo LOCK""ED; true', 30):
        break
check('lock screen spoke "Screen locked" and "Wrong password"', 'cat /tmp/lock.log', 'Wrong password')
check('... and "Screen locked"', 'cat /tmp/lock.log', 'Screen locked')
check('unlocked again', 'echo N=$(ps | grep -v grep | grep -c "my-lock --background")', 'N=0')
# 1.3.0: a window that runs as root (Disks, through my-admin) is read too
vm.run('rm -f /tmp/reader.log', 30)               # (root can't write the user's file in /tmp)
vm.run(as_user('echo root | my-admin --disks >/dev/null 2>&1 &'), 180)
time.sleep(60)
vm.keys('tab'); time.sleep(4); vm.keys('tab'); time.sleep(6)
vm.shot(SHOTS + '/05c-disks-as-root.png')
check('root window without NO_AT_BRIDGE while the reader runs',
      'p=$(pidof my-disks | cut -d" " -f1); [ -n "$p" ] && '
      'echo NAB=$(tr "\\0" "\\n" < /proc/$p/environ | grep -c NO_AT_BRIDGE) UID=$(awk "/^Uid:/{print \\$2}" /proc/$p/status)', 'NAB=0 UID=0')
out = vm.run('cat /tmp/reader.log; echo FOCUS=$(grep -v -c Disks /tmp/reader.log)', 30)
n = re.findall(r'FOCUS=(\d+)', out)              # (the echoed command has "FOCUS=$(")
results.append(('reader spoke the focus in the root window (more than its title)',
                bool(n) and int(n[-1]) > 0, out[-300:]))
print(('PASS ' if results[-1][1] else 'FAIL ') + results[-1][0], out[-300:], flush=True)
vm.run('killall my-disks; ' + as_user('my-reader --stop'), 30)
# 7. permissions: mpv may not use the camera nor see the home folder
vm.run('modprobe vivid; sleep 3; ls /dev/video*', 60)
vm.run("su dominic -c 'mkdir -p ~/.config/my-distro; echo \"ls camera=no microphone=yes home=none\" "
       "> ~/.config/my-distro/permissions'", 30)
# (0.9.9: on the live system too; until 0.9.8 bwrap failed here with
# "pivot_root: Invalid argument" and this was only checked in t_install.py)
check('camera there outside the sandbox', 'ls /dev/video*', '/dev/video')
out = vm.run(as_user('my-run ls /dev | tr \"\n\" \" \"'), 60)
body = out.split('\n', 1)[1]
results.append(('permissions: sandbox shows /dev without camera',
                'null' in body and 'video' not in body and 'pivot_root' not in body, out[-300:]))
print(('PASS ' if results[-1][1] else 'FAIL ') + results[-1][0], out[-300:], flush=True)
out = vm.run(as_user('my-run ls /dev; my-run ls ~'), 60)
body = out.split('\n', 1)[1]
results.append(('permissions: camera and home hidden',
                'null' in body and 'video' not in body and 'secret.txt' not in body and 'pivot_root' not in body,
                out[-300:]))
print(('PASS ' if results[-1][1] else 'FAIL ') + results[-1][0], out[-300:], flush=True)
# (a program without a line in the permissions file: find, not ls)
check('permissions: without a line it runs normally',
      as_user('my-run --menu "find /dev -maxdepth 1 -name video*"'), '/dev/video')
# 1.3.0: without the microphone a program still plays sound (PipeWire's
# play-only socket), but can't record or see the microphone (it sees the speakers);
# and the permissions apply when it is started by name from anywhere
# (my-run --sync's starters, first in PATH)
vm.run("su dominic -c 'printf \"speaker-test camera=yes microphone=no home=full\\n"
       "arecord camera=yes microphone=no home=full\\npw-cli camera=yes microphone=no home=full\\n\" "
       "> ~/.config/my-distro/permissions'", 30)
check('my-run --sync makes the starters', as_user('my-run --sync; ls ~/.local/share/my-run/bin | tr "\\n" " "'),
      'speaker-test')
check('the starter calls my-run with the real program', as_user('cat ~/.local/share/my-run/bin/arecord'),
      'my-run /usr/bin/arecord')
check('PipeWire\'s play-only socket', 'ls -l /run/user/1000/pipewire-0-play', 'pipewire-0-play')
vm.run(as_user('(PATH=$HOME/.local/share/my-run/bin:$PATH speaker-test -t sine -c 2 >/tmp/st.log 2>&1 &); sleep 10'), 60)
check('started by name, speaker-test runs in the sandbox', 'ps | grep -v grep | grep bwrap', 'speaker-test')
check('the sandboxed program\'s sound reaches the speakers',
      as_user('pw-link -l | grep -A2 "alsa_playback.speaker-test"'), '|->')
check('its PipeWire client is play-only', as_user('pw-cli ls Client | grep -o "my-play" | head -1'), 'my-play')
vm.run('killall speaker-test; true', 30)
out = vm.run(as_user('timeout 20 my-run pw-cli ls Node >/tmp/nodes.txt 2>&1; echo PWCLI-RC=$?; '
                       'grep -E "node.name|media.class" /tmp/nodes.txt; echo END-OF-LIST'), 60)
body = VM.body(out)
results.append(('the sandbox sees the speakers, but no microphone',
                'END-OF-LIST' in body and 'PWCLI-RC=0' in body and 'test-mic' not in body and
                'alsa_input' not in body and 'Audio/Source' not in body and 'Audio/Sink' in body, body[-300:]))
print(('PASS ' if results[-1][1] else 'FAIL ') + results[-1][0] + '\n    ' + body[-300:], flush=True)
# a recording: outside the sandbox it is linked to the microphone (the
# control), from the sandbox its stream is removed and never linked
vm.run(as_user('(/usr/bin/arecord -f cd /tmp/ctrl.wav >/dev/null 2>&1 &); sleep 8'), 60)
out = vm.run(as_user('echo LINKS=$(pw-link -l | grep -c "alsa_capture.arecord")'), 30)
n = re.findall(r'LINKS=(\d+)', out)              # (the echoed command has "LINKS=$(")
results.append(('control: an unrestricted recording is linked', bool(n) and int(n[-1]) > 0, out[-200:]))
print(('PASS ' if results[-1][1] else 'FAIL ') + results[-1][0], out[-200:], flush=True)
vm.run('killall arecord; true', 30)
vm.run(as_user('(PATH=$HOME/.local/share/my-run/bin:$PATH arecord -f cd /tmp/rec.wav >/tmp/arec.log 2>&1 &); sleep 10'), 60)
check('a sandboxed recording is never linked, its stream removed',
      as_user('echo LINKS=$(pw-link -l | grep -c "alsa_capture.arecord") '
              'NODES=$(pw-cli ls Node | grep -c "alsa_capture.arecord")'), 'LINKS=0 NODES=0')
print('arecord in the sandbox said:', vm.run('killall arecord; sleep 1; head -5 /tmp/arec.log; true', 30)[-300:],
      flush=True)
vm.run("su dominic -c 'rm -f ~/.config/my-distro/permissions'; " + as_user('my-run --sync'), 30)
check('no starters left without permissions', as_user('echo N=$(ls ~/.local/share/my-run/bin | wc -l)'), 'N=0')
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
# 1.3.0: smb:// addresses open in Network share (my-share): links (the
# x-scheme-handler) and the file manager's address bar (our PCManFM patch)
check('smb:// links open with my-share',
      as_user('gio mime x-scheme-handler/smb 2>/dev/null || grep smb /etc/xdg/mimeapps.list'), 'my-share.desktop')
vm.run(as_user('pcmanfm /tmp >/dev/null 2>&1 &'), 30)
time.sleep(30)
vm.keys('ctrl-l'); time.sleep(3)
vm.type('smb://anna@nas/files/photos\n'); time.sleep(20)
vm.shot(SHOTS + '/08b-smb-from-file-manager.png')
check('PCManFM hands smb:// to my-share', 'ps | grep -v grep | grep my-share', 'smb://anna@nas/files/photos')
vm.run('killall my-share; true', 30)
# a share that is mounted already opens at once, without the window
vm.run('mkdir -p /media/nas-files && mount -t tmpfs tmpfs /media/nas-files && mkdir /media/nas-files/photos', 30)
vm.run(as_user('my-share smb://nas/files/photos >/dev/null 2>&1 &'), 30)
time.sleep(15)
vm.shot(SHOTS + '/08c-smb-mounted-already.png')
check('a mounted share opens without asking', 'echo N=$(ps | grep -v grep | grep -c "my-share smb")', 'N=0')
vm.run('umount -l /media/nas-files; rmdir /media/nas-files; true', 30)
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
