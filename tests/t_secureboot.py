"""Secure Boot test (0.9.7): boots the ISO in QEMU with OVMF's Secure Boot
firmware and Microsoft's keys (Secure Boot on), like a new PC:

1. shim (signed by Microsoft) starts, can't check GRUB (my-distro's key is
   not enrolled) and starts MokManager: enroll the key from the ISO
   (Enroll key from disk, EFI/BOOT/my-distro-secureboot.cer), reboot
2. shim starts GRUB, GRUB the kernel: the live system comes up with
   Secure Boot on, and the kernel is locked down (integrity)
3. hibernation is refused with a message

MokManager has no serial menu we can parse reliably, so the keys are sent
blind with pauses and a screenshot after each step goes to SHOTDIR: look at
them when something fails.

    python3 tests/t_secureboot.py ISO [SHOTDIR] [STEPS]

STEPS (optional) replaces the key list for MokManager, e.g. "down,ret,ret".
Runs inside WSL; needs /usr/share/OVMF/OVMF_CODE_4M.secboot.fd and
OVMF_VARS_4M.ms.fd (Kali/Debian package ovmf)."""
import sys, time, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vm import VM

ISO = os.path.abspath(sys.argv[1])
SHOTS = sys.argv[2] if len(sys.argv) > 2 else '/tmp/vm-sb'
os.makedirs(SHOTS, exist_ok=True)
VARS = '/tmp/vm-sb-vars.fd'
subprocess.run(['cp', '/usr/share/OVMF/OVMF_VARS_4M.ms.fd', VARS], check=True)
args = ['-machine', 'q35,smm=on', '-global', 'driver=cfi.pflash01,property=secure,value=on',
        '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd',
        '-drive', 'if=pflash,format=raw,unit=1,file=' + VARS,
        '-m', '3072', '-smp', '2', '-vga', 'std', '-cdrom', ISO, '-boot', 'd',
        '-nic', 'user,model=e1000']
results = []
n = 0

def shot(tag):
    global n
    n += 1
    p = '%s/%02d-%s.png' % (SHOTS, n, tag)
    vm.shot(p)
    print('screenshot', p, flush=True)

def keys(seq, pause=2.5):
    for k in seq.split(','):
        vm.keys(k.strip())
        time.sleep(pause)

# --- 1. first start: MokManager ------------------------------------------------
vm = VM('sb', args)
time.sleep(60)                     # firmware, shim, the failed check of GRUB
shot('first-start')
# "Press any key to perform MOK management" (10 s), then the menu:
# Continue boot / Enroll key from disk / Enroll hash from disk
steps = sys.argv[3] if len(sys.argv) > 3 else None
if steps:
    keys(steps)
else:
    keys('ret', 4); shot('mok-menu')
    keys('down,ret', 4); shot('volumes')          # Enroll key from disk -> the volumes
    keys('ret', 4); shot('volume-root')           # the FAT image of the ISO
    keys('down,ret', 4); shot('efi')              # EFI/
    keys('down,ret', 4); shot('boot')             # BOOT/
    keys('end,ret', 4); shot('key')               # the last file: my-distro-secureboot.cer
    keys('down,ret', 4); shot('enroll')           # Continue
    keys('down,ret', 4); shot('confirm')          # Yes
    keys('ret', 4); shot('reboot')                # Reboot
# --- 2. the restart: GRUB, the kernel ---------------------------------------------
time.sleep(40)
shot('grub')
# the third entry sends the kernel's messages to the serial line too
keys('down,down,ret', 1)
try:
    out = vm.expect(r'(Secure boot enabled|secureboot: Secure boot enabled|Kernel is locked down)', 900)
    results.append(('kernel says Secure Boot is on', True))
    print(out[-800:], flush=True)
except TimeoutError as e:
    results.append(('kernel says Secure Boot is on', False))
    print(e, flush=True)
try:
    out = vm.expect(r'Lockdown: .*(integrity|confidentiality)', 300)
    results.append(('kernel locked down', True))
except TimeoutError:
    out = vm.buf[-3000:]
    results.append(('kernel locked down', 'lockdown' in vm.buf.lower()))
print(out[-500:], flush=True)
time.sleep(400)
shot('desktop')
# --- 3. a text console: check from there, the answers go to the serial line ----
keys('ctrl-alt-f1', 5)
vm.type('root\n'); time.sleep(3); vm.type('root\n'); time.sleep(5)
vm.type('(mokutil --sb-state; cat /sys/kernel/security/lockdown; my-power hibernate --check; '
        'my-secureboot status; echo SB-DONE) >/dev/ttyS0 2>&1\n')
try:
    out = vm.expect('SB-DONE', 120)
    print(out[-1200:], flush=True)
    results.append(('mokutil: SecureBoot enabled', 'SecureBoot enabled' in out))
    results.append(('lockdown integrity', '[integrity]' in out))
    results.append(('hibernate refused with a reason', 'Secure Boot' in out))
    results.append(('key enrolled', 'enrolled = yes' in out))
except TimeoutError as e:
    print(e, flush=True)
    results.append(('console checks', False))
shot('console')
print('\n== SUMMARY secureboot')
for name, ok in results:
    print(('PASS ' if ok else 'FAIL ') + name)
vm.quit()
