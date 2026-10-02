"""Loopback-only fixture for the actual photo editor HTML; never contacts a device."""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from email.parser import BytesParser
from email.policy import default
import json
import re

sketch = Path(__file__).resolve().parents[1]
uploads = []
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def reply(self, code, data, mime='application/json'):
        self.send_response(code)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        if self.path == '/':
            page = (sketch/'robot_photo_ui.h').read_text(encoding='utf-8').split('R"HTML(',1)[1].split(')HTML";',1)[0]
            head = '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><style>body{font:16px system-ui;margin:24px;max-width:700px;background:#f6f8fa;color:#102230}fieldset{border:1px solid #ccd6df;border-radius:12px;padding:20px;margin:16px 0}button,input{font:inherit;padding:9px;margin:6px 0;max-width:100%;box-sizing:border-box}input[type=range]{width:100%}.actions{display:flex;gap:8px;flex-wrap:wrap}.hint{font-size:14px;color:#546675}#photo-crop[hidden]{display:none}</style>'
            css = re.search(r'<style>(.*?)</style>', (sketch/'web_admin_ui.h').read_text(encoding='utf-8'), re.S)[1]
            self.reply(200, (head+'<style>'+css+'body{margin:24px;max-width:700px}</style>'+page+'</html>').encode(), 'text/html; charset=utf-8')
        elif self.path == '/display/photo/status':
            self.reply(200,json.dumps({'ready':True,'width':320,'height':240,'count':len(uploads),'capacity':8,'current':0,'items':[]}).encode())
        elif self.path == '/fixture/uploads':
            self.reply(200,json.dumps(uploads).encode())
        else: self.reply(404,b'{}')
    def do_POST(self):
        if self.path != '/display/photo': self.reply(404,b'{}'); return
        body = self.rfile.read(int(self.headers.get('Content-Length',0)))
        message = BytesParser(policy=default).parsebytes(('Content-Type: '+self.headers['Content-Type']+'\r\n\r\n').encode()+body)
        data = next(p for p in message.iter_parts() if p.get_param('name',header='content-disposition') == 'photo').get_payload(decode=True)
        import hashlib
        uploads.append({'bytes':len(data),'header':list(data[:8]),'sha256':hashlib.sha256(data).hexdigest()})
        self.reply(200,b'{"ok":true}')

print('Photo crop fixture: http://127.0.0.1:8791',flush=True)
HTTPServer(('127.0.0.1',8791),Handler).serve_forever()
