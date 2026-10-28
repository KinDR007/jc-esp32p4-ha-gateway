#!/usr/bin/env python3
"""Watch attribute changes of one Matter node through the Matter Server WebSocket API.

Logs every attribute update and event of the node (old -> new), highlights the watched
cluster and periodically reads its attributes. Standard library only.

Example:
    python3 tools/matter_watch.py --url ws://homeassistant.local:5580/ws --node 39 \
        --cluster 0x117CFC01 --log liljebagge.jsonl
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
from urllib.parse import urlsplit


class WebSocket:
    def __init__(self, url, timeout=10):
        u = urlsplit(url)
        if u.scheme != "ws":
            raise ValueError("only ws:// URLs are supported")
        self.sock = socket.create_connection((u.hostname, u.port or 80), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        host = f"[{u.hostname}]" if ":" in u.hostname else u.hostname
        req = (f"GET {u.path or '/'} HTTP/1.1\r\nHost: {host}:{u.port or 80}\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("connection closed during handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n", 1)[0].decode(errors="replace")
        if " 101 " not in status:
            raise ConnectionError(f"handshake failed: {status}")
        self.sock.settimeout(None)
        self.lock = threading.Lock()

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def _send_frame(self, opcode, payload):
        hdr = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        with self.lock:
            self.sock.sendall(hdr + mask + body)

    def send(self, obj):
        self._send_frame(0x1, json.dumps(obj).encode())

    def recv(self):
        message = b""
        while True:
            b0, b1 = self._read(2)
            opcode, n = b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            mask = self._read(4) if b1 & 0x80 else None
            payload = self._read(n)
            if mask:
                payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            if opcode == 0x8:
                raise ConnectionError("server closed the connection")
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            message += payload
            if b0 & 0x80:
                return json.loads(message)


def ts():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--url", default="ws://homeassistant.local:5580/ws")
    ap.add_argument("--node", type=int, required=True)
    ap.add_argument("--cluster", default="0x117CFC01", help="cluster to highlight and poll")
    ap.add_argument("--endpoint", type=int, default=1)
    ap.add_argument("--attrs", default="0,1", help="attribute IDs of the cluster to poll")
    ap.add_argument("--interval", type=int, default=300, help="poll period in s, 0 = off")
    ap.add_argument("--log", help="append JSON lines to this file")
    args = ap.parse_args()

    cluster = int(args.cluster, 0)
    prefix = f"{args.endpoint}/{cluster}/"
    poll_paths = [prefix + a.strip() for a in args.attrs.split(",") if a.strip()]
    logf = open(args.log, "a") if args.log else None

    def log(kind, path, old, new):
        mark = "  <== FC" if path.startswith(prefix) else ""
        if kind == "initial":
            print(f"{ts()} {path} = {json.dumps(new)}{mark}", flush=True)
        elif kind == "event":
            print(f"{ts()} EVENT {path}: {json.dumps(new)}{mark}", flush=True)
        elif old != new:
            print(f"{ts()} {path}: {json.dumps(old)} -> {json.dumps(new)}{mark}", flush=True)
        if logf and (old != new or kind == "event"):
            logf.write(json.dumps({"t": ts(), "kind": kind, "path": path,
                                   "old": old, "new": new}) + "\n")
            logf.flush()

    state = {}

    def session(ws):
        info = ws.recv()
        print(f"{ts()} server: {info.get('sdk_version', info)}", flush=True)
        ws.send({"message_id": "listen", "command": "start_listening"})
        while True:
            msg = ws.recv()
            if msg.get("message_id") == "listen":
                break
        if "error_code" in msg:
            sys.exit(f"start_listening failed: {msg}")
        node = next((n for n in msg["result"] if n["node_id"] == args.node), None)
        if node is None:
            sys.exit(f"node {args.node} not found")
        print(f"{ts()} node {args.node} available={node.get('available')}", flush=True)
        if state:
            for path, value in node["attributes"].items():
                log("resync", path, state.get(path), value)
        else:
            for path in sorted(p for p in node["attributes"] if p.startswith(prefix)):
                log("initial", path, None, node["attributes"][path])
        state.update(node["attributes"])

        pending = {}
        stop = threading.Event()

        def poller():
            n = 0
            while not stop.wait(args.interval):
                for path in poll_paths:
                    n += 1
                    mid = f"read{n}"
                    pending[mid] = path
                    try:
                        ws.send({"message_id": mid, "command": "read_attribute",
                                 "args": {"node_id": args.node, "attribute_path": path}})
                    except OSError:
                        return

        if args.interval > 0 and poll_paths:
            threading.Thread(target=poller, daemon=True).start()
        try:
            while True:
                msg = ws.recv()
                event = msg.get("event")
                if event == "attribute_updated":
                    node_id, path, value = msg["data"]
                    if node_id == args.node:
                        log("update", path, state.get(path), value)
                        state[path] = value
                elif event == "node_event":
                    data = msg["data"]
                    if data.get("node_id") == args.node:
                        path = f"{data.get('endpoint_id')}/{data.get('cluster_id')}/e{data.get('event_id')}"
                        log("event", path, None, data.get("data"))
                elif event in ("node_updated", "node_removed") and isinstance(msg.get("data"), dict) \
                        and msg["data"].get("node_id") == args.node:
                    print(f"{ts()} {event} available={msg['data'].get('available')}", flush=True)
                elif msg.get("message_id") in pending:
                    path = pending.pop(msg["message_id"])
                    if "error_code" in msg:
                        print(f"{ts()} read {path} failed: {msg.get('details')}", flush=True)
                        continue
                    result = msg.get("result")
                    value = result.get(path) if isinstance(result, dict) else result
                    log("read", path, state.get(path), value)
                    state[path] = value
        finally:
            stop.set()

    connected_once = False
    try:
        while True:
            try:
                ws = WebSocket(args.url)
            except (OSError, ConnectionError) as e:
                if not connected_once:
                    sys.exit(f"cannot connect to {args.url}: {e}\n"
                             "Expose port 5580 in the Matter Server add-on (Configuration -> Network).")
                time.sleep(10)
                continue
            connected_once = True
            try:
                session(ws)
            except (OSError, ConnectionError) as e:
                print(f"{ts()} disconnected: {e}, reconnecting", flush=True)
                ws.sock.close()
                time.sleep(10)
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
