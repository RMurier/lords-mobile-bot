#!/usr/bin/env python3
"""Lists the packets of the game server's TCP flow in a capture of the official client.

    gcc -shared -fPIC -Iinclude -o /tmp/libdes.so src/des.c        # once: the bot's own DES
    python3 tools/decode_capture.py capture.pcapng --lib /tmp/libdes.so [--only 3202,1433] [--full]

Client -> server payloads are DES-encrypted with ENCRYPTION_KEY (src/des.c) and are printed decrypted; server -> client payloads
are in clear. Each direction is reassembled on its own, so the two lists are in order within themselves but not interleaved with
each other (a server answer follows the request it answers, in the same relative order). Message names: include/packet_map.h.
The login packet carries the session key: the capture is as sensitive as the account. Delete it when done.
"""
import argparse
import collections
import ctypes
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_credentials as ec  # pcap/pcapng reader and TCP reassembly

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def packet_names():
    text = open(os.path.join(ROOT, "include", "packet_map.h"), encoding="utf-8").read()
    return {int(m.group(1)): m.group(2) for m in re.finditer(r'\{\s*(\d+),\s*"(\w+)"\s*\}', text)}


def split_packets(stream):
    """u16 total size (header included), u16 type, payload."""
    pos, out = 0, []
    while pos + 4 <= len(stream):
        size, kind = struct.unpack("<HH", stream[pos:pos + 4])
        if size < 4 or pos + size > len(stream):
            break
        out.append((kind, stream[pos + 4:pos + size]))
        pos += size
    return out


GAME_PORT = 10013


def find_start(stream, names, need=6, limit=20000):
    """Offset where a chain of `need` packets in a row (u16 size, u16 known type) begins, in a stream taken in the middle of a connection."""
    for start in range(min(limit, max(0, len(stream) - 4))):
        pos, count = start, 0
        while count < need and pos + 4 <= len(stream):
            size, kind = struct.unpack("<HH", stream[pos:pos + 4])
            if size < 4 or pos + size > len(stream) or kind not in names:
                break
            pos += size
            count += 1
        if count >= need:
            return start
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--lib", required=True, help="shared library built from src/des.c")
    ap.add_argument("--only", help="comma separated message numbers to show")
    ap.add_argument("--full", action="store_true", help="print whole payloads (default: first 64 bytes)")
    args = ap.parse_args()

    lib = ctypes.CDLL(args.lib)
    key = (ctypes.c_uint8 * 8).in_dll(lib, "ENCRYPTION_KEY")

    def decrypt(data):
        n = len(data)
        out = (ctypes.c_uint8 * n)()
        lib.DecryptData((ctypes.c_uint8 * n).from_buffer_copy(data), n, out, key)
        return bytes(out)

    flows = collections.defaultdict(list)
    for linktype, frame in ec.read_capture(args.capture):
        parsed = ec.parse_tcp(ec.strip_link_layer(linktype, frame))
        if parsed:
            flows[parsed[0]].append((parsed[1], parsed[2]))

    game = None
    for flow, segments in flows.items():
        stream = ec.reassemble(segments)
        if any(kind == ec.LOGIN_GAME for kind, _ in ec.parse_login(stream)):
            game = flow
    names = packet_names()
    resync = game is None
    if resync:
        # The capture began after the game had logged in: no login packet to point at the flow. Take the flow to the game server port (10013) that
        # carried the most data, and find where the packets start in the middle of each stream.
        best = max((f for f in flows if f[3] == GAME_PORT), key=lambda f: sum(len(p) for _, p in flows[f]), default=None)
        if best is None:
            sys.exit("no game server login (message 1044) and no flow to the game port %d in this capture: was it started before the game?" % GAME_PORT)
        game = best
        print("note: no login in this capture (it started after the game): the packets are found by their chain of sizes and known message numbers", file=sys.stderr)

    def packets_of(stream):
        return split_packets(stream[find_start(stream, names):]) if resync else split_packets(stream)

    reverse = (game[2], game[3], game[0], game[1])
    only = {int(x) for x in args.only.split(",")} if args.only else None
    for label, flow, encrypted in (("CLIENT -> SERVER", game, True), ("SERVER -> CLIENT", reverse, False)):
        print(f"== {label} ({ec.ip_to_str(flow[0])}:{flow[1]} -> {ec.ip_to_str(flow[2])}:{flow[3]})")
        for kind, payload in packets_of(ec.reassemble(flows.get(flow, []))):
            if only and kind not in only:
                continue
            body = decrypt(payload) if encrypted and kind != ec.LOGIN_GAME else payload
            shown = body if args.full else body[:64]
            print(f"{kind:>6} {names.get(kind, '?'):<50} {len(payload):>5}  {shown.hex(' ')}{'' if args.full or len(body) <= 64 else ' ...'}")


if __name__ == "__main__":
    main()
