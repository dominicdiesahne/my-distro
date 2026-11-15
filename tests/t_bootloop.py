"""Boots a disk that t_install.py left behind N times with BIOS, only up to
the passphrase prompt (crypt) or the kernel (plain), to catch a rare hang
in GRUB: python3 t_bootloop.py plain|crypt [N] [BUILD]

t_install.py put GRUB's serial terminal and a marker at the top of the
installed boot menu, so each boot is checked in two steps: GRUB read its
menu, then the kernel / initramfs got going. A step that doesn't come in
time stops the loop with screenshots of the screen and of GRUB's "set",
"ls" and its boot menu in /tmp/vm-bootloop-MODE (as t_install does).

Runs inside WSL, one VM at a time. The disk is only read until the prompt."""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

mode = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 10
BUILD = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/my-distro-build-hw')
DISK = BUILD + '/test-%s.img' % mode
SHOTS = '/tmp/vm-bootloop-' + mode
GRUB_MARK = 'MY-GRUB-READ-ITS-MENU'
os.makedirs(SHOTS, exist_ok=True)
args = ['-m', '2048', '-smp', '4', '-vga', 'std', '-nic', 'user,model=e1000',
        '-drive', 'file=%s,if=virtio,format=raw,readonly=on' % DISK]
ok = 0

def failed(v, i, what):
    v._pump()
    print('FAIL boot %d: %s; serial line:\n%s' % (i, what, v.buf[-3000:]), flush=True)
    print('screenshot', v.shot('%s/%02d-1-screen.png' % (SHOTS, i)), flush=True)
    v.keys('c'); time.sleep(3)
    for j, cmd in enumerate(('set', 'ls', 'cat $prefix/grub.cfg')):
        v.type(cmd + '\n'); time.sleep(8)
        print('screenshot', v.shot('%s/%02d-%d-%s.png' % (SHOTS, i, j + 2, cmd.split()[0])), flush=True)
    v._pump(); print(v.buf[-3000:], flush=True)
    v.quit()
    print('\n== SUMMARY bootloop %s\nPASS %d of %d boots\nFAIL boot %d: %s' % (mode, ok, N, i, what))
    sys.exit(1)

for i in range(1, N + 1):
    t0 = time.time()
    v = VM('bootloop-' + mode, args)
    try:
        v.expect(GRUB_MARK, 900)
    except (TimeoutError, RuntimeError):
        failed(v, i, 'GRUB did not read its boot menu')
    t1 = time.time()
    try:
        v.expect('Enter passphrase for' if mode == 'crypt' else 'Linux version', 1500)
    except (TimeoutError, RuntimeError):
        failed(v, i, 'no passphrase prompt / kernel after GRUB')
    ok += 1
    print('boot %d: GRUB menu after %d s, %s after %d s' % (i, t1 - t0, 'prompt' if mode == 'crypt' else 'kernel',
                                                          time.time() - t0), flush=True)
    v.quit()

print('\n== SUMMARY bootloop %s\nPASS %d of %d boots' % (mode, ok, N))
