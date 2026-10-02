"""Local-only browser fixture: serves generated forms and echoes submitted fields.

This does not emulate firmware auth, NVS or Wi-Fi; it only checks browser form
behavior using HTML produced by the actual C++ PageStream code.
"""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
import json
import sys

root = Path(sys.argv[1]).resolve()
class Handler(BaseHTTPRequestHandler):
    def reply(self, code, data, content_type):
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == '/display/expressions':
            self.reply(200,(root/'expressions.json').read_bytes(),'application/json; charset=utf-8'); return
        if url.path == '/display/expressions/image':
            query = parse_qs(url.query)
            value = query.get('id',[''])[0]
            if not value.isdigit() or not 0 <= int(value) < 48:
                self.reply(422,b'Invalid ID','text/plain'); return
            style = 'body' if query.get('body') == ['1'] else 'eyes'
            self.reply(200,(root/f'thumb-{style}-{int(value)}.bmp').read_bytes(),'image/bmp'); return
        if url.path == '/favicon.ico':
            self.reply(204,b'','image/x-icon'); return
        name = {'/':'controls.html', '/controls':'controls.html','/setup':'setup.html'}.get(url.path)
        if not name:
            self.reply(404,b'', 'text/plain'); return
        self.reply(200,(root/name).read_bytes(),'text/html; charset=utf-8')
    def do_POST(self):
        size = int(self.headers.get('Content-Length', '0'))
        if size > 4096:
            self.reply(413,b'', 'text/plain'); return
        data = {'path':self.path, 'fields':parse_qs(self.rfile.read(size).decode())}
        self.reply(200,json.dumps(data, ensure_ascii=False).encode(),'application/json; charset=utf-8')

print('Display form fixture: http://127.0.0.1:8789', flush=True)
HTTPServer(('127.0.0.1',8789),Handler).serve_forever()
