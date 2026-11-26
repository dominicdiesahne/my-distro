"""A pretend Windows PC for find-servers (WS-Discovery): answers Probes on
239.255.255.250:3702 with a ProbeMatch, and the metadata Get on
http://ADDRESS:5357/ with the computer name.

python3 wsd_fake.py ADDRESS NAME SECONDS
"""
import socket, struct, sys, threading, time, uuid
from http.server import BaseHTTPRequestHandler, HTTPServer

ADDR, NAME, SECONDS = sys.argv[1], sys.argv[2], float(sys.argv[3])
DEV = 'urn:uuid:%s' % uuid.uuid4()

MATCH = '''<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
 xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
 xmlns:wsd="http://schemas.xmlsoap.org/ws/2005/04/discovery">
<soap:Header><wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action></soap:Header>
<soap:Body><wsd:ProbeMatches><wsd:ProbeMatch>
<wsa:EndpointReference><wsa:Address>%s</wsa:Address></wsa:EndpointReference>
<wsd:Types>wsdp:Device pub:Computer</wsd:Types>
<wsd:XAddrs>http://%s:5357/%s</wsd:XAddrs>
</wsd:ProbeMatch></wsd:ProbeMatches></soap:Body></soap:Envelope>'''

META = '''<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
 xmlns:pub="http://schemas.microsoft.com/windows/pub/2005/07">
<soap:Body><wsx:Metadata xmlns:wsx="http://schemas.xmlsoap.org/ws/2004/09/mex">
<pub:Computer>%s/Workgroup:WORKGROUP</pub:Computer>
</wsx:Metadata></soap:Body></soap:Envelope>'''


class Meta(BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        body = (META % NAME).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/soap+xml')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


http = HTTPServer((ADDR, 5357), Meta)
threading.Thread(target=http.serve_forever, daemon=True).start()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', 3702))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
             struct.pack('4s4s', socket.inet_aton('239.255.255.250'), socket.inet_aton(ADDR)))
s.settimeout(0.5)
out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
out.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
out.bind((ADDR, 3702))                      # (answers come from the PC's address)
end = time.time() + SECONDS
while time.time() < end:
    try:
        data, peer = s.recvfrom(65536)
    except socket.timeout:
        continue
    if b'/discovery/Probe<' in data:
        out.sendto((MATCH % (DEV, ADDR, DEV[9:])).encode(), peer)
        print('answered', peer, flush=True)
