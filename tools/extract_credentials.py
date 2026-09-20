#!/usr/bin/env python3
"""
Extract account credentials from a network capture of YOUR OWN device.

The login packets sent by the game are in clear text. They carry everything the
bot needs:

    _MSG_NEWLOGIN_LOGINTOL (1043), sent to the gateway:
        igg_id, client version, language, device uuid (empty on the PC client),
        access key (session)
    _MSG_NEWLOGIN_LOGINTOP (1044), sent to the game server:
        igg_id and the same access key

Capture the game while it starts (Wireshark, or `pktmon` on Windows), then run:

    python3 tools/extract_credentials.py capture.pcapng --template config.cfg --out-dir accounts/

One config file is written per IGG ID found in the capture, ready to use with
`client accounts/<igg_id>.cfg`. The gateway address seen in the capture is
written as server.addr / server.port.

Only the Python standard library is used. Supports pcap and pcapng, Ethernet /
raw IP / Linux cooked captures, IPv4 and IPv6. Captures taken through a VPN
adapter (raw IP frames labelled as Ethernet) are handled.

The output contains live session credentials. Keep it private and never commit
it (accounts/ and *.pcap* are in .gitignore).
"""

import argparse
import os
import re
import socket
import struct
import sys

LOGIN_SIZE = 2 + 2 + 8 + 1 + 1 + 2 + 1 + 1 + 50 + 2 + 512  # 582
LOGIN_GATEWAY = 1043  # _MSG_NEWLOGIN_LOGINTOL
LOGIN_GAME = 1044     # _MSG_NEWLOGIN_LOGINTOP
LOGIN_TYPES = (LOGIN_GATEWAY, LOGIN_GAME)

LINKTYPE_NULL = 0
LINKTYPE_ETHERNET = 1
LINKTYPE_RAW = 101
LINKTYPE_LINUX_SLL = 113
LINKTYPE_IPV4 = 228
LINKTYPE_IPV6 = 229
LINKTYPE_LINUX_SLL2 = 276


# --------------------------------------------------------------------------
# Capture file readers: yield (linktype, frame_bytes)
# --------------------------------------------------------------------------

def read_pcap(data):
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        raise ValueError("not a pcap file")

    linktype = struct.unpack(endian + "I", data[20:24])[0] & 0x0FFFFFFF
    pos = 24
    while pos + 16 <= len(data):
        incl_len = struct.unpack(endian + "I", data[pos + 8:pos + 12])[0]
        pos += 16
        if incl_len > len(data) - pos:
            break
        yield linktype, data[pos:pos + incl_len]
        pos += incl_len


def read_pcapng(data):
    endian = "<"
    interfaces = []
    pos = 0
    while pos + 12 <= len(data):
        block_type = struct.unpack(endian + "I", data[pos:pos + 4])[0]
        if block_type == 0x0A0D0D0A:  # section header: detect endianness
            bom = data[pos + 8:pos + 12]
            endian = "<" if bom == b"\x4d\x3c\x2b\x1a" else ">"
            interfaces = []
        block_len = struct.unpack(endian + "I", data[pos + 4:pos + 8])[0]
        if block_len < 12 or pos + block_len > len(data):
            break
        body = data[pos + 8:pos + block_len - 4]

        if block_type == 0x00000001:  # interface description
            interfaces.append(struct.unpack(endian + "H", body[0:2])[0])
        elif block_type == 0x00000006 and len(body) >= 20:  # enhanced packet
            if_id, _, _, cap_len = struct.unpack(endian + "IIII", body[0:16])
            if if_id < len(interfaces):
                yield interfaces[if_id], body[20:20 + cap_len]
        elif block_type == 0x00000003 and interfaces:  # simple packet
            cap_len = struct.unpack(endian + "I", body[0:4])[0]
            yield interfaces[0], body[4:4 + cap_len]

        pos += block_len


def read_capture(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"\x0a\x0d\x0d\x0a":
        return read_pcapng(data)
    return read_pcap(data)


# --------------------------------------------------------------------------
# Frame -> TCP segment
# --------------------------------------------------------------------------

def looks_like_ip(frame):
    """True if the bytes start like an IPv4 or IPv6 header (no link layer)."""
    if len(frame) >= 20 and frame[0] >> 4 == 4 and (frame[0] & 0x0F) >= 5:
        return True
    return len(frame) >= 40 and frame[0] >> 4 == 6


def strip_link_layer(linktype, frame):
    """Return the IP packet inside a link-layer frame, or None."""
    if linktype == LINKTYPE_ETHERNET:
        if len(frame) >= 14:
            ethertype = struct.unpack(">H", frame[12:14])[0]
            offset = 14
            while ethertype in (0x8100, 0x88A8) and len(frame) >= offset + 4:  # VLAN
                ethertype = struct.unpack(">H", frame[offset + 2:offset + 4])[0]
                offset += 4
            if ethertype in (0x0800, 0x86DD):
                return frame[offset:]
        # VPN adapters (Wintun/WireGuard...) show up as raw IP labelled Ethernet
        return frame if looks_like_ip(frame) else None
    if linktype == LINKTYPE_LINUX_SLL:
        return frame[16:] if len(frame) > 16 else None
    if linktype == LINKTYPE_LINUX_SLL2:
        return frame[20:] if len(frame) > 20 else None
    if linktype == LINKTYPE_NULL:
        return frame[4:] if len(frame) > 4 else None
    if linktype in (LINKTYPE_RAW, LINKTYPE_IPV4, LINKTYPE_IPV6):
        return frame
    return None


def parse_tcp(ip):
    """Return (flow_key, seq, payload) for a TCP packet, or None."""
    if not ip:
        return None
    version = ip[0] >> 4
    if version == 4:
        if len(ip) < 20:
            return None
        ihl = (ip[0] & 0x0F) * 4
        total_len = struct.unpack(">H", ip[2:4])[0]
        frag = struct.unpack(">H", ip[6:8])[0]
        if ip[9] != 6 or (frag & 0x1FFF) != 0:
            return None
        src, dst = ip[12:16], ip[16:20]
        tcp = ip[ihl:total_len if 0 < total_len <= len(ip) else len(ip)]
    elif version == 6:
        if len(ip) < 40:
            return None
        next_header = ip[6]
        src, dst = ip[8:24], ip[24:40]
        offset = 40
        while next_header in (0, 43, 60):  # hop-by-hop, routing, destination opts
            if len(ip) < offset + 8:
                return None
            next_header, ext_len = ip[offset], ip[offset + 1]
            offset += (ext_len + 1) * 8
        if next_header != 6:
            return None
        tcp = ip[offset:]
    else:
        return None

    if len(tcp) < 20:
        return None
    sport, dport, seq = struct.unpack(">HHI", tcp[0:8])
    data_offset = (tcp[12] >> 4) * 4
    payload = tcp[data_offset:]
    if not payload:
        return None
    return (src, sport, dst, dport), seq, payload


def reassemble(segments):
    """Rebuild a byte stream from (seq, payload) pairs, tolerating retransmits."""
    if not segments:
        return b""
    base = segments[0][0]
    placed = {}
    for seq, payload in segments:
        rel = (seq - base) & 0xFFFFFFFF
        if rel > 0x7FFFFFFF:  # earlier than the first segment we saw
            rel -= 0x100000000
        placed[rel] = max(placed.get(rel, b""), payload, key=len)

    lowest = min(placed)
    stream = bytearray()
    for rel in sorted(placed):
        payload = placed[rel]
        pos = rel - lowest
        if pos > len(stream):
            stream += b"\x00" * (pos - len(stream))  # gap: pad, scanning is signature based
        stream[pos:pos + len(payload)] = payload
    return bytes(stream)


def ip_to_str(raw):
    return socket.inet_ntop(socket.AF_INET if len(raw) == 4 else socket.AF_INET6, raw)


# --------------------------------------------------------------------------
# Login packets
# --------------------------------------------------------------------------

def parse_login(stream):
    """Yield (packet_type, dict) for every login packet found in a TCP byte stream."""
    for ptype in LOGIN_TYPES:
        header = struct.pack("<HH", LOGIN_SIZE, ptype)
        pos = 0
        while True:
            pos = stream.find(header, pos)
            if pos < 0:
                break
            packet = stream[pos:pos + LOGIN_SIZE]
            pos += 1
            if len(packet) < LOGIN_SIZE:
                continue

            igg_id = struct.unpack("<Q", packet[4:12])[0]
            session_len = struct.unpack("<H", packet[68:70])[0]
            if igg_id == 0 or session_len == 0 or session_len > 512:
                continue
            try:
                uuid = packet[18:68].split(b"\x00", 1)[0].decode("ascii", errors="strict")
                session = packet[70:70 + session_len].decode("ascii", errors="strict")
            except UnicodeDecodeError:
                continue
            if not session.isprintable() or not uuid.isprintable():
                continue

            info = {"igg_id": igg_id, "access_key": session, "device_uuid": uuid}
            if ptype == LOGIN_GATEWAY:
                info["version_minor"] = packet[12]
                info["version_major"] = packet[13]
                info["version_patch"] = struct.unpack("<H", packet[14:16])[0]
                info["language_code"] = packet[17]
                info["platform"] = packet[16]
            yield ptype, info


def extract(path):
    flows = {}
    for linktype, frame in read_capture(path):
        parsed = parse_tcp(strip_link_layer(linktype, frame))
        if parsed:
            key, seq, payload = parsed
            flows.setdefault(key, []).append((seq, payload))

    accounts = {}
    for (src, sport, dst, dport), segments in flows.items():
        for ptype, login in parse_login(reassemble(segments)):
            account = accounts.setdefault(login["igg_id"], {"igg_id": login["igg_id"]})
            account["access_key"] = login["access_key"]
            if ptype == LOGIN_GATEWAY:
                # Gateway login carries the version, language, device uuid and
                # tells us which gateway this client talks to.
                account["device_uuid"] = login["device_uuid"]
                for field in ("version_major", "version_minor", "version_patch", "language_code", "platform"):
                    account[field] = login[field]
                account["server_addr"] = ip_to_str(dst)
                account["server_port"] = dport
            else:
                account.setdefault("device_uuid", login["device_uuid"])
    return list(accounts.values())


# --------------------------------------------------------------------------
# Config output
# --------------------------------------------------------------------------

def render_config(template_text, account, data_dir):
    replacements = {
        "account.igg_id": str(account["igg_id"]),
        "account.access_key": account["access_key"],
    }
    for key, field in (("client.version_major", "version_major"),
                       ("client.version_minor", "version_minor"),
                       ("client.version_patch", "version_patch"),
                       ("client.language_code", "language_code"),
                       ("client.platform", "platform"),
                       ("server.addr", "server_addr"),
                       ("server.port", "server_port")):
        if field in account:
            replacements[key] = str(account[field])
    if data_dir:
        replacements["data.path"] = data_dir

    # The PC client sends an empty device uuid. The config parser needs a value,
    # so leave the key out (the bot then sends zeros, like the PC client does).
    uuid = account.get("device_uuid", "")
    uuid_line = f"account.device_uuid = {uuid}" if uuid else "# account.device_uuid is empty on this client"

    seen = set()
    lines = []
    for line in template_text.splitlines():
        match = re.match(r"^\s*([A-Za-z0-9_.]+)\s*=", line)
        key = match.group(1) if match else None
        if key == "account.device_uuid":
            lines.append(uuid_line)
            seen.add(key)
        elif key in replacements:
            lines.append(f"{key} = {replacements[key]}")
            seen.add(key)
        else:
            lines.append(line)

    missing = [k for k in replacements if k not in seen]
    if uuid and "account.device_uuid" not in seen:
        missing.append("account.device_uuid")
        replacements["account.device_uuid"] = uuid
    if missing:
        lines.append("")
        lines.append("# Added by extract_credentials.py")
        lines.extend(f"{k} = {replacements[k]}" for k in missing)
    return "\n".join(lines) + "\n"


def write_private(path, text):
    """Write a file readable by the current user only (POSIX)."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", newline="\n") as f:
        f.write(text)


def mask(value, keep=4):
    return value[:keep] + "…" + f"({len(value)} chars)" if len(value) > keep else "***"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("capture", help="pcap or pcapng file of your own device logging in")
    parser.add_argument("--template", default="config.cfg",
                        help="config to copy other settings from (create one with: client --create-config)")
    parser.add_argument("--out-dir", default="accounts", help="where to write one .cfg per account")
    parser.add_argument("--show-secrets", action="store_true",
                        help="print full credentials instead of masked values")
    args = parser.parse_args()

    if not os.path.isfile(args.template):
        sys.exit(f"Template config not found: {args.template}\n"
                 f"Create one first with: client --create-config")
    with open(args.template, encoding="utf-8") as f:
        template = f.read()

    try:
        accounts = extract(args.capture)
    except (OSError, ValueError, struct.error) as e:
        sys.exit(f"Could not read capture: {e}")

    if not accounts:
        sys.exit("No login packet found (_MSG_NEWLOGIN_LOGINTOL / LOGINTOP). Start the capture "
                 "BEFORE launching the game and only stop it once you are in game, so that the "
                 "connection to the gateway is included.")

    os.makedirs(args.out_dir, exist_ok=True)
    for account in accounts:
        name = str(account["igg_id"])
        out_path = os.path.join(args.out_dir, name + ".cfg")
        write_private(out_path, render_config(template, account, f"./data/{name}/"))

        key = account["access_key"] if args.show_secrets else mask(account["access_key"])
        uuid = account.get("device_uuid", "")
        uuid = (uuid if args.show_secrets else mask(uuid)) if uuid else "(empty)"
        if "version_major" in account:
            print(f"IGG ID {name}: client v{account['version_major']}.{account['version_minor']}."
                  f"{account['version_patch']}, lang {account['language_code']}, platform {account['platform']}")
        else:
            print(f"IGG ID {name}: gateway login not in capture, version/server left from template")
        if "server_addr" in account:
            print(f"  gateway     = {account['server_addr']}:{account['server_port']}")
        print(f"  device_uuid = {uuid}")
        print(f"  access_key  = {key}")
        print(f"  -> {out_path}")

    print(f"\n{len(accounts)} account(s) written. Review admin.name and the other settings "
          f"in each file before starting the bot.")


if __name__ == "__main__":
    main()
