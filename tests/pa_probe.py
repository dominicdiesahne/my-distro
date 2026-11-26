"""A minimal PulseAudio client (the native protocol, no libpulse) for t_live.

python3 pa_probe.py SOCKET [SECONDS]

Connects, authenticates, names itself with pipewire.client.access =
"unrestricted" in its property list (a play-only client must not get out
that way: desktop/patches/pipewire), lists the sinks and sources and says
whether the speakers (alsa_output) and the microphone (alsa_input) were
among them, then stays connected for SECONDS so the test can look at its
PipeWire client.
"""
import socket, struct, sys, time

AUTH, SET_CLIENT_NAME, SINKS, SOURCES, REPLY, ERROR = 8, 9, 22, 24, 2, 0


def u32(v):
    return b'L' + struct.pack('>I', v)


def string(s):
    return b't' + s.encode() + b'\0'


def proplist(d):
    out = b'P'
    for k, v in d.items():
        data = v.encode() + b'\0'
        out += string(k) + u32(len(data)) + b'x' + struct.pack('>I', len(data)) + data
    return out + b'N'


class Pulse:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(10)
        self.s.connect(path)
        self.tag = 0

    def recv_exact(self, n):
        b = b''
        while len(b) < n:
            c = self.s.recv(n - len(b))
            if not c:
                raise EOFError('server closed the connection')
            b += c
        return b

    def call(self, cmd, payload=b''):
        self.tag += 1
        body = u32(cmd) + u32(self.tag) + payload
        self.s.sendall(struct.pack('>IIIII', len(body), 0xffffffff, 0, 0, 0) + body)
        while True:
            n, channel = struct.unpack('>IIIII', self.recv_exact(20))[:2]
            data = self.recv_exact(n)
            if channel != 0xffffffff:
                continue                    # (memblocks: none expected)
            rcmd, rtag = struct.unpack('>I', data[1:5])[0], struct.unpack('>I', data[6:10])[0]
            if rtag != self.tag:
                continue                    # (events)
            if rcmd == ERROR:
                raise RuntimeError('error reply to command %d' % cmd)
            return data


p = Pulse(sys.argv[1])
p.call(AUTH, u32(35) + b'x' + struct.pack('>I', 256) + b'\0' * 256)
p.call(SET_CLIENT_NAME, proplist({'application.name': 'pa-probe',
                                  'pipewire.client.access': 'unrestricted'}))
time.sleep(2)                               # (WirePlumber sets the permissions)
sinks, sources = p.call(SINKS), p.call(SOURCES)
print('SINKS-HAVE-OUTPUT=%d SOURCES-HAVE-INPUT=%d' % (b'alsa_output' in sinks, b'alsa_input' in sources),
      flush=True)
time.sleep(float(sys.argv[2]) if len(sys.argv) > 2 else 0)
