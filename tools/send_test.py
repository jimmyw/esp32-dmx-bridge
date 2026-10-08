#!/usr/bin/env python3
"""Send test patterns to the DMX bridge via Art-Net or sACN, or discover Art-Net nodes.

Examples:
  send_test.py poll                              # ArtPoll broadcast, list nodes
  send_test.py artnet 192.168.1.50 --universe 0  # unicast Art-Net chase
  send_test.py sacn --universe 1                 # multicast sACN ramp
  send_test.py sacn 192.168.1.50 --pattern full  # unicast sACN, all channels 255
"""
import argparse
import math
import socket
import struct
import sys
import time
import uuid

ARTNET_PORT = 6454
SACN_PORT = 5568


def pattern(name, t, n=512):
    if name == "chase":
        pos = int(t * 8) % n
        return bytes(255 if i == pos else 0 for i in range(n))
    if name == "ramp":
        base = int(t * 64)
        return bytes((base + i) & 0xFF for i in range(n))
    if name == "sine":
        return bytes(int(127.5 + 127.5 * math.sin(t * 2 + i / 8)) for i in range(n))
    if name == "full":
        return bytes([255] * n)
    if name == "off":
        return bytes(n)
    raise SystemExit(f"unknown pattern {name}")


def artdmx(universe, seq, data):
    return (b"Art-Net\0" + struct.pack("<H", 0x5000) + struct.pack(">H", 14) +
            bytes([seq, 0]) + struct.pack("<H", universe) + struct.pack(">H", len(data)) + data)


def e131(universe, seq, data, cid, priority=100, name="send_test.py", terminate=False):
    slots = b"\x00" + data
    dmp_len = 10 + len(slots)
    framing_len = 77 + dmp_len
    root_len = 22 + framing_len
    pkt = struct.pack(">HH12s", 0x0010, 0, b"ASC-E1.17\0\0\0")
    pkt += struct.pack(">HI16s", 0x7000 | root_len, 0x00000004, cid)
    pkt += struct.pack(">HI64sBHBBH", 0x7000 | framing_len, 0x00000002,
                       name.encode()[:63], priority, 0, seq, 0x40 if terminate else 0, universe)
    pkt += struct.pack(">HBBHHH", 0x7000 | dmp_len, 0x02, 0xA1, 0, 1, len(slots))
    return pkt + slots


def cmd_poll(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", ARTNET_PORT))
    s.settimeout(0.5)
    s.sendto(b"Art-Net\0" + struct.pack("<H", 0x2000) + struct.pack(">H", 14) + b"\x02\x00",
             (args.target or "255.255.255.255", ARTNET_PORT))
    end = time.time() + 3
    found = 0
    while time.time() < end:
        try:
            d, a = s.recvfrom(1024)
        except socket.timeout:
            continue
        if d[:8] != b"Art-Net\0" or struct.unpack("<H", d[8:10])[0] != 0x2100:
            continue
        found += 1
        ip = socket.inet_ntoa(d[10:14])
        short = d[26:44].split(b"\0")[0].decode(errors="replace")
        long_ = d[44:108].split(b"\0")[0].decode(errors="replace")
        report = d[108:172].split(b"\0")[0].decode(errors="replace")
        port_addr = (d[18] << 8) | (d[19] << 4) | d[190]
        print(f"{ip:15}  {short:18} universe {port_addr:5}  {long_}  [{report}]")
    if not found:
        print("no Art-Net nodes answered")


def cmd_send(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
    if args.cmd == "artnet":
        dst = (args.target or "255.255.255.255", ARTNET_PORT)
        universe = 0 if args.universe is None else args.universe
    else:
        universe = 1 if args.universe is None else args.universe
        dst = (args.target or f"239.255.{universe >> 8}.{universe & 0xFF}", SACN_PORT)
    cid = uuid.uuid4().bytes
    print(f"{args.cmd} '{args.pattern}' -> {dst[0]} universe {universe} @ {args.fps} fps (Ctrl+C to stop)")
    seq = 0
    t0 = time.time()
    try:
        while True:
            data = pattern(args.pattern, time.time() - t0)
            seq = (seq + 1) & 0xFF
            pkt = artdmx(universe, seq or 1, data) if args.cmd == "artnet" else \
                e131(universe, seq, data, cid, args.priority)
            s.sendto(pkt, dst)
            if args.duration and time.time() - t0 > args.duration:
                break
            time.sleep(1 / args.fps)
    except KeyboardInterrupt:
        pass
    if args.cmd == "sacn":
        for _ in range(3):  # E1.31: announce stream termination
            seq = (seq + 1) & 0xFF
            s.sendto(e131(universe, seq, bytes(512), cid, args.priority, terminate=True), dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["artnet", "sacn", "poll"])
    ap.add_argument("target", nargs="?", help="device IP (default: broadcast / multicast)")
    ap.add_argument("--universe", type=int)
    ap.add_argument("--pattern", default="chase", choices=["chase", "ramp", "sine", "full", "off"])
    ap.add_argument("--fps", type=float, default=30)
    ap.add_argument("--priority", type=int, default=100, help="sACN priority 0-200")
    ap.add_argument("--duration", type=float, default=0, help="seconds, 0 = until Ctrl+C")
    args = ap.parse_args()
    cmd_poll(args) if args.cmd == "poll" else cmd_send(args)


if __name__ == "__main__":
    sys.exit(main())
