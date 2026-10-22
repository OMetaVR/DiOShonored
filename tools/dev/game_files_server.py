#!/usr/bin/env python3
"""Serve YOUR OWN Dishonored install to your own phone (dev builds of the iOS app download it from here over your LAN/Tailscale).
  python3 -I tools/dev/game_files_server.py <Dishonored install dir> [--port 8791]
GET /manifest.tsv -> "<size>\\t<relpath>" for every file the port needs (Binaries/Win32 is reduced to the recompiled modules);
GET /f/<relpath>  -> the file, with Range support so interrupted downloads resume. Only manifest paths are served. Read-only."""
import os, sys, urllib.parse
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
root = os.path.abspath(sys.argv[1])
port = int(sys.argv[sys.argv.index('--port') + 1]) if '--port' in sys.argv else 8791
MODULES = {'Binaries/Win32/Dishonored.exe', 'Binaries/Win32/Microsoft.VC90.CRT/msvcr90.dll', 'Binaries/Win32/Microsoft.VC90.CRT/msvcp90.dll',
           'Binaries/Win32/binkw32.dll', 'Binaries/Win32/PhysXLoader.dll', 'Binaries/Win32/PhysXCore.dll', 'Binaries/Win32/PhysXCooking.dll'}
files = {}
for d, _, fs in os.walk(root):
    for f in fs:
        rel = os.path.relpath(os.path.join(d, f), root).replace(os.sep, '/')
        if rel.startswith('Binaries/') and rel not in MODULES: continue
        if rel in ('installscript.vdf',): continue
        files[rel] = os.path.getsize(os.path.join(root, rel))
manifest = ''.join('%d\t%s\n' % (files[r], r) for r in sorted(files)).encode()
print('serving %d files, %.2f GB from %s on port %d' % (len(files), sum(files.values()) / 1e9, root, port), flush=True)

served = set()

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, fmt, *a): pass
    def do_GET(self):
        if self.path == '/manifest.tsv':
            print('%s fetched the manifest' % self.client_address[0], flush=True)
            self.send_response(200); self.send_header('Content-Length', str(len(manifest))); self.send_header('Content-Type', 'text/plain')
            self.end_headers(); self.wfile.write(manifest); return
        if not self.path.startswith('/f/'):
            self.send_error(404); return
        rel = urllib.parse.unquote(self.path[3:])
        if rel not in files:
            self.send_error(404); return
        size = files[rel]; start = 0
        rng = self.headers.get('Range')
        if rng and rng.startswith('bytes='):
            start = int(rng[6:].split('-')[0] or 0)
        if start > size:
            self.send_error(416); return
        if start:
            self.send_response(206); self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, size - 1, size))
        else:
            self.send_response(200)
        self.send_header('Content-Length', str(size - start)); self.send_header('Content-Type', 'application/octet-stream'); self.end_headers()
        with open(os.path.join(root, rel), 'rb') as f:
            f.seek(start)
            while True:
                b = f.read(1 << 20)
                if not b: break
                self.wfile.write(b)
        served.add(rel)
        print('%s sent %s (%d bytes from %d)  [%d/%d files sent this session]' % (self.client_address[0], rel, size - start, start, len(served), len(files)), flush=True)
ThreadingHTTPServer(('0.0.0.0', port), H).serve_forever()
