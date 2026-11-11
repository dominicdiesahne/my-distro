"""Small QEMU harness for my-distro tests (runs inside WSL).

    vm = VM(name, qemu_args)          starts QEMU with a monitor and a serial
                                      socket in /tmp/vm-NAME/
    vm.expect(pattern, timeout)       waits for a regex on the serial line
    vm.send(text)                     types on the serial line
    vm.keys("ctrl-alt-f1"), vm.type("text\n")   keys on the VGA console (monitor sendkey)
    vm.shot(path)                     screenshot as PNG
    vm.quit()
"""
import os, re, socket, subprocess, time, zlib, struct

KEYMAP = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '.': 'dot', ',': 'comma',
          '/': 'slash', ';': 'semicolon', "'": 'apostrophe', '\t': 'tab', '_': 'shift-minus',
          ':': 'shift-semicolon', '"': 'shift-apostrophe', '!': 'shift-1', '@': 'shift-2',
          '|': 'shift-backslash', '>': 'shift-dot', '<': 'shift-comma', '*': 'shift-8',
          '$': 'shift-4', '&': 'shift-7', '(': 'shift-9', ')': 'shift-0', '+': 'shift-equal',
          '?': 'shift-slash', '\\': 'backslash', '~': 'shift-grave_accent', '#': 'shift-3',
          '%': 'shift-5', '[': 'bracket_left', ']': 'bracket_right', '{': 'shift-bracket_left',
          '}': 'shift-bracket_right', '^': 'shift-6'}

class VM:
    def __init__(self, name, args, log=None):
        self.dir = '/tmp/vm-' + name
        os.makedirs(self.dir, exist_ok=True)
        for f in ('mon', 'ser'):
            try: os.remove(os.path.join(self.dir, f))
            except OSError: pass
        cmd = ['qemu-system-x86_64'] + args + [
            '-monitor', 'unix:%s/mon,server,nowait' % self.dir,
            '-serial', 'unix:%s/ser,server,nowait' % self.dir]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(self.dir + '/qemu.err', 'w'))
        for _ in range(100):
            if os.path.exists(self.dir + '/mon') and os.path.exists(self.dir + '/ser'):
                break
            time.sleep(0.1)
        time.sleep(0.5)
        self.mon = socket.socket(socket.AF_UNIX); self.mon.connect(self.dir + '/mon')
        self.ser = socket.socket(socket.AF_UNIX); self.ser.connect(self.dir + '/ser')
        self.ser.setblocking(False)
        self.buf = ''
        self.log = open(log or self.dir + '/serial.log', 'a')
        self._mon_read()

    def _mon_read(self, t=0.3):
        self.mon.settimeout(t); out = b''
        try:
            while True:
                d = self.mon.recv(65536)
                if not d: break
                out += d
        except (socket.timeout, BlockingIOError):
            pass
        return out.decode(errors='replace')

    def monitor(self, cmd):
        self.mon.sendall((cmd + '\n').encode()); time.sleep(0.2)
        return self._mon_read()

    def _pump(self):
        try:
            while True:
                d = self.ser.recv(65536)
                if not d: break
                t = d.decode(errors='replace'); self.buf += t; self.log.write(t); self.log.flush()
        except (BlockingIOError, socket.timeout):
            pass

    def expect(self, pattern, timeout=600):
        end = time.time() + timeout; rx = re.compile(pattern)
        while time.time() < end:
            self._pump()
            m = rx.search(self.buf)
            if m:
                out = self.buf[:m.end()]; self.buf = self.buf[m.end():]
                return out
            if self.proc.poll() is not None:
                raise RuntimeError('qemu exited')
            time.sleep(0.5)
        raise TimeoutError('timeout waiting for %r; last output: %r' % (pattern, self.buf[-500:]))

    def send(self, text):
        self.ser.sendall(text.encode())

    def run(self, cmd, timeout=300, prompt=r'\n[^\n]*[#$] $'):
        """Runs a shell command on the serial console; returns its output."""
        mark = 'END%d' % int(time.time() * 1000)
        # a command ending in "&" can't be followed by ";" (syntax error)
        sep = '\n' if cmd.rstrip().endswith('&') else '; '
        self.send(cmd + sep + 'echo ' + mark + ' $?\n')
        out = self.expect(mark + r' (\d+)', timeout)
        return out

    def keys(self, combo):
        self.monitor('sendkey ' + combo)

    def type(self, text):
        for ch in text:
            if ch.isalpha() and ch.isupper(): k = 'shift-' + ch.lower()
            elif ch.isalnum(): k = ch
            else: k = KEYMAP[ch]
            self.monitor('sendkey ' + k)
            time.sleep(0.05)

    def shot(self, png):
        ppm = png + '.ppm'
        self.monitor('screendump ' + ppm); time.sleep(1.5)
        data = open(ppm, 'rb').read(); os.remove(ppm)
        parts = data.split(b'\n', 3)
        w, h = map(int, parts[1].split()); pix = parts[3]
        raw = b''.join(b'\0' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
        def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        open(png, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
        return png

    def quit(self):
        try: self.monitor('quit')
        except OSError: pass
        try: self.proc.wait(20)
        except subprocess.TimeoutExpired: self.proc.kill()
