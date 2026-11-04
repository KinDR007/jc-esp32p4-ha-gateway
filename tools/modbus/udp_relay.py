#!/usr/bin/env python3
"""UDP relay for testing: lets the gateway reach a device in a network only this computer reaches (e.g. over
a VPN). Datagrams to --listen go to --target; the target's answers go back to whoever sent last.

  udp_relay.py --target 192.168.1.170:8899 [--listen 18899]

On the gateway: a device on connection "GoodWe UDP" with this computer as host and the listen port."""
import argparse
import select
import socket
import sys
import time


def addr(text, default_host=''):
    host, _, port = text.rpartition(':')
    return host or default_host, int(port)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--target', required=True, help='host:port of the device')
    p.add_argument('--listen', default='18899', help='[host:]port to listen on (default 18899)')
    p.add_argument('--quiet', action='store_true', help='no line per datagram')
    a = p.parse_args()
    target = addr(a.target)
    down = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    down.bind(addr(a.listen, '0.0.0.0'))
    up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    up.connect(target)  # only the target's datagrams arrive here
    client = None
    print(f'relay {down.getsockname()[0]}:{down.getsockname()[1]} -> {target[0]}:{target[1]}', flush=True)
    while True:
        ready, _, _ = select.select([down, up], [], [])
        for s in ready:
            try:
                if s is down:
                    data, client = down.recvfrom(2048)
                    up.send(data)
                    direction = f'{client[0]} -> target'
                else:
                    data = up.recv(2048)
                    if not client:
                        continue
                    down.sendto(data, client)
                    direction = f'target -> {client[0]}'
            except OSError as e:  # e.g. the target unreachable (ICMP): keep running
                print(f'{time.strftime("%H:%M:%S")} {e}', flush=True)
                continue
            if not a.quiet:
                print(f'{time.strftime("%H:%M:%S")} {direction} {len(data)} B {data[:8].hex()}', flush=True)


if __name__ == '__main__':
    sys.exit(main())
