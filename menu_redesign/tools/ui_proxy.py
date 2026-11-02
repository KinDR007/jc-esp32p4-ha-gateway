"""Serves a local web UI file and forwards the API to the gateway, read only (GET), with an API token.

Lets the reworked UI run against live data without flashing the gateway; POST/PUT/DELETE are refused,
so taking screenshots cannot change the gateway. Wi-Fi names and BSSIDs (the ESPHome radio's network, the
Wi-Fi networks of a scan) are masked.
  P4GW_TOKEN=<admin token> python3 ui_proxy.py <index.html> [gateway=192.168.88.243] [port=8088]
"""
import http.server
import json
import os
import sys
import urllib.error
import urllib.request

PAGE = sys.argv[1]
GATEWAY = sys.argv[2] if len(sys.argv) > 2 else '192.168.88.243'
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8088
TOKEN = os.environ['P4GW_TOKEN']


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/' or self.path.startswith('/?') or self.path == '/index.html':
            body = open(PAGE, 'rb').read()
            self.reply(200, 'text/html; charset=utf-8', body)
            return
        req = urllib.request.Request(f'http://{GATEWAY}{self.path}', headers={'Authorization': f'Bearer {TOKEN}'})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                body = r.read()
                if self.path.startswith('/api/esphome/wifi'):
                    w = json.loads(body)
                    w['ssid'] = 'HomeWiFi' if w.get('ssid') else ''
                    body = json.dumps(w).encode()
                elif self.path.startswith('/api/net/scan'):
                    d = json.loads(body)
                    names = {}
                    for i, ap in enumerate((d.get('last') or {}).get('wifi') or []):
                        if ap.get('ssid'):
                            ap['ssid'] = names.setdefault(ap['ssid'], f'WiFi-{len(names) + 1}')
                        ap['bssid'] = f'02000000{i:04x}'
                    body = json.dumps(d).encode()
                self.reply(r.status, r.headers.get('Content-Type', 'application/octet-stream'), body)
        except urllib.error.HTTPError as e:
            self.reply(e.code, e.headers.get('Content-Type', 'text/plain'), e.read())
        except OSError as e:
            self.reply(502, 'text/plain', str(e).encode())

    def refuse(self):
        self.reply(403, 'application/json', b'{"error":"read-only preview"}')

    do_POST = do_PUT = do_DELETE = do_PATCH = refuse

    def reply(self, code, ctype, body):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
