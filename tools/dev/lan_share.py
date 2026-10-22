#!/usr/bin/env python3
"""Tiny LAN file drop: serves a file (or every file in a directory) and accepts uploads from the phone.
usage: lan_share.py <file_or_dir_to_serve> <upload_dir> [host] [port]"""
import html, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from email.parser import BytesParser
from email.policy import default

SERVE, UPDIR = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
HOST = sys.argv[3] if len(sys.argv) > 3 else "0.0.0.0"
PORT = int(sys.argv[4]) if len(sys.argv) > 4 else 8765
os.makedirs(UPDIR, exist_ok=True)

def served_files():
    if os.path.isdir(SERVE):
        return {n: os.path.join(SERVE, n) for n in sorted(os.listdir(SERVE)) if os.path.isfile(os.path.join(SERVE, n))}
    return {os.path.basename(SERVE): SERVE}

def page():
    links = "".join(f'<p><a href="/{html.escape(n)}">Download {html.escape(n)}</a></p>' for n in served_files())
    return f"""<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<h2>Dishonored iOS dev drop</h2>{links}
<form method=post action=/upload enctype=multipart/form-data>
<p>Send results back:</p><input type=file name=f multiple><button>Upload</button></form>"""

class H(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        files = served_files()
        name = self.path.lstrip("/")
        if name in files:
            with open(files[name], "rb") as fh: data = fh.read()
            self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="{name}"')
            self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)
        else:
            self._send(200, page().encode())

    def do_POST(self):
        if self.path != "/upload":
            return self._send(404, b"not found")
        n = int(self.headers.get("Content-Length", 0))
        raw = b"Content-Type: " + self.headers["Content-Type"].encode() + b"\r\n\r\n" + self.rfile.read(n)
        saved = []
        for part in BytesParser(policy=default).parsebytes(raw).iter_parts():
            fn = part.get_filename()
            if not fn: continue
            fn = os.path.basename(fn).replace("..", "_")
            with open(os.path.join(UPDIR, fn), "wb") as fh: fh.write(part.get_payload(decode=True))
            saved.append(fn)
        print("uploaded:", saved, flush=True)
        self._send(200, (page() + "<p>Saved: " + html.escape(", ".join(saved) or "nothing") + "</p>").encode())

print(f"serving http://{HOST}:{PORT}/  ({', '.join(served_files())}) -> uploads to {UPDIR}", flush=True)
ThreadingHTTPServer((HOST, PORT), H).serve_forever()
