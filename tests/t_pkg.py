"""Software app package tests (0.9.7): boots the live system, installs
packages from out/packages through a local web server (my-pkg), and tries
them: tcc compiles and runs hello.c, node, docker run hello-world (QEMU's
user network reaches the internet), Samba sharing to itself, ClamAV finds
a test file.

    python3 tests/t_pkg.py PKGDIR [BUILD]

PKGDIR: the folder make packages wrote (out/packages); it is served on the
host at port 8000, which the VM reaches as 10.0.2.2. Runs inside WSL."""
import sys, time, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

PKGS = os.path.abspath(sys.argv[1])
BUILD = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/my-distro-build-hw')
O = BUILD + '/desktop-out/images'
results = []

def check(name, cmd, want, timeout=600):
    out = vm.run(cmd, timeout)
    body = out.split('\n', 1)[1] if '\n' in out else out
    ok = want in body
    results.append((name, ok))
    print(('PASS ' if ok else 'FAIL ') + name + '\n    ' + body.strip()[-600:].replace('\n', '\n    '), flush=True)
    return body

# The release's package list is signed with the maintainer's key (make
# release-packages), which tests don't have: sign the list with a throw-away
# key, and put its public half into the VM (the real key is never touched).
import shutil
SERVE = '/tmp/pkg-serve'
shutil.rmtree(SERVE, ignore_errors=True); os.makedirs(SERVE)
for n in os.listdir(PKGS):
    if not n.endswith('.sig'):
        os.symlink(os.path.join(PKGS, n), os.path.join(SERVE, n))
subprocess.run(['openssl', 'genpkey', '-algorithm', 'ed25519', '-out', '/tmp/pkg-test-priv.pem'], check=True)
subprocess.run(['openssl', 'pkey', '-in', '/tmp/pkg-test-priv.pem', '-pubout', '-out', SERVE + '/test-key.pem'], check=True)
subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin', '-inkey', '/tmp/pkg-test-priv.pem',
                '-in', PKGS + '/my-pkg-index.json', '-out', SERVE + '/my-pkg-index.json.sig'], check=True)
server = subprocess.Popen(['python3', '-m', 'http.server', '8000', '--directory', SERVE],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
vm = VM('pkg', ['-m', '4096', '-smp', '4', '-kernel', O + '/bzImage', '-initrd', O + '/rootfs.cpio.gz',
                '-append', 'rdinit=/sbin/my-init initramfs_options=size=90% console=tty1 console=ttyS0,115200',
                '-vga', 'std', '-nic', 'user,model=e1000'])
try:
    vm.expect(r'login: ', 1800)
    vm.send('root\n'); vm.expect('Password: ', 60); vm.send('root\n'); vm.expect(r'# ', 120)
    vm.send('stty cols 200; export PS1="# "\n'); vm.expect(r'# ', 30)
    vm.run("sed -i 's|^url *=.*|url = http://10.0.2.2:8000/|' /etc/my-distro/pkg.conf; "
           "grep -q '^url' /etc/my-distro/pkg.conf || echo 'url = http://10.0.2.2:8000/' >> /etc/my-distro/pkg.conf", 30)
    vm.run("python3 -c \"import urllib.request as u; open('/etc/my-distro/update-key.pem','wb')"
           ".write(u.urlopen('http://10.0.2.2:8000/test-key.pem').read())\"", 60)
    check('package list', 'my-pkg list | head -50', 'tinycc')

    # 10. a C compiler
    check('install tinycc', 'my-pkg install tinycc 2>&1 | tail -3', 'tinycc')
    vm.run("printf '#include <stdio.h>\\n#include <math.h>\\nint main(void) { printf(\"Hello from tcc %%d\\\\n\", "
           "(int)sqrt(1764.0)); return 0; }\\n' > /tmp/hello.c; cat /tmp/hello.c", 30)
    check('tcc compiles and runs hello.c', 'tcc /tmp/hello.c -o /tmp/hello -lm && /tmp/hello', 'Hello from tcc 42')
    check('tcc -run', 'tcc -run /tmp/hello.c -lm', 'Hello from tcc 42')
    check('tcc as a normal user', "su dominic -c 'cd /tmp && tcc hello.c -o h2 -lm && ./h2'", 'Hello from tcc 42')

    # 11. Node.js (if it was built)
    if 'nodejs' in vm.run('my-pkg list', 60):
        check('install nodejs', 'my-pkg install nodejs 2>&1 | tail -3', 'nodejs', timeout=1200)
        check('node runs', "node -e 'console.log(6 * 7, process.version)'", '42')
    else:
        print('SKIP nodejs (not in the package list)', flush=True)

    # 10. Docker
    check('install docker-engine', 'my-pkg install docker-engine 2>&1 | tail -3', 'docker', timeout=1200)
    vm.run('/etc/init.d/S60docker start; sleep 30', 120)
    check('dockerd runs', 'docker info 2>&1 | grep -E "Server Version|Storage Driver|Cgroup"', 'Server Version')
    check('docker run hello-world', 'docker run --rm hello-world 2>&1 | head -5', 'Hello from Docker', timeout=900)
    check('container reaches the internet', 'docker run --rm busybox wget -q -O /dev/null -T 20 '
          "http://example.com && echo NET''OK", 'NETOK', timeout=900)

    # 2. Samba sharing
    check('install samba4', 'my-pkg install samba4 2>&1 | tail -3', 'samba', timeout=1200)
    vm.run("su dominic -c 'mkdir -p ~/Public && echo shared-ok > ~/Public/note.txt'", 30)
    check('sharing on', "echo sharepass1 | /usr/lib/my-distro/sharing on dominic /home/dominic/Public rw printers", 'DONE')
    time.sleep(10)
    check('sharing status', '/usr/lib/my-distro/sharing status', 'running = yes')
    check('firewall opened 445', 'nft list set inet my-firewall sharing_tcp', '445')
    check('smbclient reads the share', "smbclient //127.0.0.1/Public -U dominic%sharepass1 -c 'get note.txt /tmp/got.txt' "
          '2>&1 | tail -2; cat /tmp/got.txt', 'shared-ok')
    check('wrong password refused', "smbclient //127.0.0.1/Public -U dominic%nope -c ls 2>&1 | tail -1", 'NT_STATUS_LOGON_FAILURE')
    check('printers in smb.conf', 'grep -A3 "\\[printers\\]" /etc/samba/smb.conf', 'printable')
    check('sharing off', '/usr/lib/my-distro/sharing off; nft list set inet my-firewall sharing_tcp', 'DONE')
    out = vm.run('nft list set inet my-firewall sharing_tcp; pidof smbd || echo NOSMBD', 30)
    results.append(('firewall closed and smbd gone', '445' not in out.split('\n', 1)[1] and 'NOSMBD' in out))
    print(results[-1], out[-300:], flush=True)

    # 8. ClamAV
    check('install clamav', 'my-pkg install clamav 2>&1 | tail -3', 'clamav', timeout=1200)
    # the EICAR test file (harmless; every virus scanner reports it), base64
    vm.run("echo WDVPIVAlQEFQWzRcUFpYNTQoUF4pN0NDKTd9JEVJQ0FSLVNUQU5EQVJELUFOVElWSVJVUy1URVNULUZJTEUhJEgrSCo= "
           "| base64 -d > /tmp/eicar.com; cat /tmp/eicar.com; echo; "
           "python3 -c \"print('MyTest.EICAR:0:*:' + b'EICAR-STANDARD-ANTIVIRUS-TEST-FILE'.hex())\" > /tmp/t.ndb; cat /tmp/t.ndb", 30)
    check('clamscan finds the test file (own signature)', 'clamscan --no-summary -d /tmp/t.ndb /tmp/eicar.com', 'FOUND')
    check('freshclam reaches the signature mirror', "mkdir -p /tmp/cdb; printf 'DatabaseDirectory /tmp/cdb\\nDatabaseMirror "
          "database.clamav.net\\nDatabaseOwner root\\n' > /tmp/fc.conf; timeout 900 freshclam --config-file=/tmp/fc.conf "
          "--stdout 2>&1 | grep -q 'cvd updated' && echo FRESH''OK", 'FRESHOK', timeout=1000)
    check('clamscan with the real signatures', 'clamscan --no-summary -d /tmp/cdb /tmp/eicar.com 2>&1 | tail -2', 'FOUND',
          timeout=900)
    check('remove packages again', 'my-pkg remove tinycc 2>&1 | tail -2; command -v tcc || echo GONE', 'GONE')
finally:
    print('\n== SUMMARY packages')
    for n, ok in results:
        print(('PASS ' if ok else 'FAIL ') + n)
    vm.quit()
    server.terminate()
