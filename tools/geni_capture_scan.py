#!/usr/bin/env python3
"""Reassembling GENIbus scanner for resources/traffic_capture.

Why this exists. The corpus README documents a trap that has cost this project
hours twice: the negotiated MTU leaves 20 bytes of ATT payload, reads are
11-byte telegrams and fit in one packet, and **every write is longer and spans
two**. A scanner that looks for CRC-valid frames inside individual packets
therefore finds every read and not one write, and the corpus reads as a pump
nobody ever writes to. That conclusion was reached twice, once confidently
enough to reach code comments, a changelog and a GitHub issue.

So this reassembles before it scans: HCI ACL (honouring the PB continuation
flag) -> L2CAP CID 0x0004 -> ATT opcode + handle -> value, concatenated per
(acl_handle, att_handle, direction). The reassembly is self-checking --
`--coverage` reports the fraction of each stream consumed by CRC-valid frames,
and a sound parse is ~100% with nothing left over.

It also drops the duplicate files. Three captures are the same session as
another and two carry no ACL data at all; counting every file inflates totals by
roughly 29%.

Usage:
    tools/geni_capture_scan.py sets        # Class 10 SET census by address
    tools/geni_capture_scan.py gets        # Class 10 GET census by address
    tools/geni_capture_scan.py acks        # short Class 10 reply census
    tools/geni_capture_scan.py order       # SET order per file, obj/sub
    tools/geni_capture_scan.py coverage    # per-file reassembly soundness

Reads resources/traffic_capture relative to the repo root; pass --dir to point
it elsewhere. Formats: Apple PacketLogger (.pklg, either endianness) and
btsnoop (.log/.pcap).

Note that `resources/` is gitignored -- the captures are local reverse
engineering material, not part of the distribution -- so this script ships
without the corpus it reads. It is checked in anyway because the analysis it
performs is what the protocol comments throughout `components/` cite, and those
citations are worth being reproducible by anyone holding the captures.
"""

from __future__ import annotations

import argparse
import glob
import os
import struct
import sys

# Same session as another capture, or no ACL data at all. From the corpus
# README, and independently reproduced here: hashing each file's frame set makes
# the first three 100% contained in their twin.
DUPLICATES = {
    "pump_start_stop_2.log",  # == pump_start_stop_2.pcap == 1.24.26 6.02.39 PM.pklg
    "pump_start_stop_2.pcap",
    "start_stop_pump.log",  # == start_stop.pklg
    "grundfos_authentication.pklg",  # strict substring of 10.31.25 2.30.14 PM.pklg
    "mode_settings.log",  # no ACL data
    "10.31.25 2.29.26 PM.pklg",  # no ACL data
}

CAPTURE_EXTENSIONS = (".pklg", ".log", ".pcap")


def records_btsnoop(blob):
    """Yield (is_receive, acl_bytes) from a btsnoop file (datalink 1001, H1)."""
    off = 16  # 'btsnoop\0' + version(4) + datalink(4)
    while off + 24 <= len(blob):
        _olen, ilen, flags, _drops, _ts = struct.unpack_from(">IIIIq", blob, off)
        off += 24
        data = blob[off : off + ilen]
        off += ilen
        if flags & 0x02:  # command/event, not ACL data
            continue
        yield bool(flags & 0x01), data


def records_pklg(blob):
    """Yield (is_receive, acl_bytes) from an Apple PacketLogger file."""
    off, end = 0, len(blob)
    while off + 13 <= end:
        big = struct.unpack_from(">I", blob, off)[0]
        little = struct.unpack_from("<I", blob, off)[0]
        length = big if 9 <= big <= 70000 else little
        if not 9 <= length <= 70000:
            return
        kind = blob[off + 12]
        data = blob[off + 13 : off + 4 + length]
        off += 4 + length
        if kind == 0x02:
            yield False, data  # ACL out: host -> controller
        elif kind == 0x03:
            yield True, data  # ACL in


def load(path):
    blob = open(path, "rb").read()
    if blob[:8] == b"btsnoop\x00":
        return list(records_btsnoop(blob))
    return list(records_pklg(blob))


def att_streams(records):
    """Concatenated ATT values, keyed (acl_handle, att_handle, is_receive)."""
    partial, streams = {}, {}
    for recv, data in records:
        if len(data) < 4:
            continue
        header, total = struct.unpack_from("<HH", data, 0)
        pb = (header >> 12) & 0x3
        key = (header & 0x0FFF, recv)
        payload = data[4 : 4 + total]
        if pb in (0b10, 0b00):
            partial[key] = bytearray(payload)
        elif pb == 0b01:
            partial.setdefault(key, bytearray()).extend(payload)
        else:
            continue
        buf = partial.get(key)
        if buf is None or len(buf) < 4:
            continue
        l2len, cid = struct.unpack_from("<HH", buf, 0)
        if len(buf) < 4 + l2len:
            continue  # more fragments still coming
        pdu = bytes(buf[4 : 4 + l2len])
        del partial[key]
        # write req / write cmd / notification / indication
        if cid != 0x0004 or len(pdu) < 3 or pdu[0] not in (0x12, 0x52, 0x1B, 0x1D):
            continue
        att_handle = struct.unpack_from("<H", pdu, 1)[0]
        streams.setdefault((key[0], att_handle, recv), bytearray()).extend(pdu[3:])
    return streams


def crc16(data):
    """CRC-CCITT, 0xFFFF init, final complement -- as codec.cpp computes it."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc ^ 0xFFFF


def frames(stream):
    """CRC-valid GENIbus telegrams in a byte stream: [SD][LEN][DA][SA][APDU..][CRC]."""
    out, i, n = [], 0, len(stream)
    while i < n:
        if stream[i] not in (0x24, 0x27) or i + 2 > n:
            i += 1
            continue
        total = stream[i + 1] + 4
        if i + total > n:
            i += 1
            continue
        frame = bytes(stream[i : i + total])
        if crc16(frame[1:-2]) == struct.unpack(">H", frame[-2:])[0]:
            out.append(frame)
            i += total
        else:
            i += 1
    return out


def captures(directory, include_duplicates=False):
    for path in sorted(glob.glob(os.path.join(directory, "*"))):
        name = os.path.basename(path)
        if os.path.splitext(path)[1] not in CAPTURE_EXTENSIONS:
            continue
        if not include_duplicates and name in DUPLICATES:
            continue
        yield name, path


def apdus(directory, include_duplicates=False):
    """Yield (file, is_receive, apdu_bytes) for every CRC-valid frame."""
    for name, path in captures(directory, include_duplicates):
        for (_h, _ah, recv), buf in sorted(att_streams(load(path)).items()):
            for frame in frames(buf):
                yield name, recv, frame[4:-2]


def address(body):
    """(object, sub-id, type, version) from a Class 10 request body."""
    obj = body[0]
    sub = struct.unpack(">H", body[1:3])[0]
    typ = struct.unpack(">H", body[3:5])[0] if len(body) >= 5 else None
    ver = body[5] if len(body) >= 6 else None
    return obj, sub, typ, ver


def cmd_sets(args):
    census, files = {}, {}
    for name, recv, apdu in apdus(args.dir):
        if recv or len(apdu) < 5 or apdu[0] != 0x0A or (apdu[1] >> 6) != 2:
            continue
        body = apdu[2 : 2 + (apdu[1] & 0x3F)]
        key = address(body) + (apdu[1],)
        census[key] = census.get(key, 0) + 1
        files.setdefault(key, set()).add(name)
    print(f"{'obj':>4} {'sub':>5} {'type':>5} {'ver':>3} {'opspec':>6} {'count':>6}  files")
    for key in sorted(census):
        obj, sub, typ, ver, opspec = key
        print(f"{obj:4d} {sub:5d} {str(typ):>5} {str(ver):>3}   0x{opspec:02X} {census[key]:6d}  {len(files[key])}")
    print(f"\n{sum(census.values())} SETs, {len(census)} distinct address shapes")


def cmd_gets(args):
    census = {}
    for _name, recv, apdu in apdus(args.dir):
        if recv or len(apdu) < 3 or apdu[0] != 0x0A or (apdu[1] >> 6) != 0:
            continue
        body = apdu[2 : 2 + (apdu[1] & 0x3F)]
        obj, sub, _t, _v = address(body)
        census[(obj, sub)] = census.get((obj, sub), 0) + 1
    for (obj, sub), n in sorted(census.items()):
        print(f"  obj {obj:3d} sub {sub:5d}  x{n}")
    print(f"\n{sum(census.values())} GETs, {len(census)} distinct addresses")


def cmd_acks(args):
    ACK = {0: "ok", 1: "Unknown Class", 2: "Unknown Data Item", 3: "Illegal Operation"}
    CLASS10 = {0: "OK", 2: "BUSY", 4: "OPERATION_FAILED"}
    census, n_sets = {}, 0
    for _name, recv, apdu in apdus(args.dir):
        if len(apdu) < 2 or apdu[0] != 0x0A:
            continue
        if not recv:
            n_sets += (apdu[1] >> 6) == 2
            continue
        plen = apdu[1] & 0x3F
        if plen > 2:
            continue  # a data reply, not the short acknowledge shape
        first = apdu[2] if plen >= 1 and len(apdu) > 2 else None
        census[(apdu[1], first)] = census.get((apdu[1], first), 0) + 1
    print(f"Class 10 SETs sent: {n_sets}")
    for (head, first), n in sorted(census.items()):
        status = CLASS10.get(first, "-" if first is None else f"0x{first:02X}")
        print(f"  head 0x{head:02X} ({ACK[(head >> 6) & 3]:<18}) len={head & 0x3F} status={status:<16} n={n}")
    print(f"  {sum(census.values())} short replies")


def cmd_order(args):
    for name, path in captures(args.dir):
        seq = []
        for (_h, _ah, recv), buf in sorted(att_streams(load(path)).items()):
            if recv:
                continue
            for frame in frames(buf):
                apdu = frame[4:-2]
                if len(apdu) < 5 or apdu[0] != 0x0A or (apdu[1] >> 6) != 2:
                    continue
                obj, sub, _t, _v = address(apdu[2 : 2 + (apdu[1] & 0x3F)])
                seq.append(f"{obj}/{sub}")
        if seq:
            print(f"--- {name}\n    " + " ".join(seq))


def cmd_coverage(args):
    for name, path in captures(args.dir, include_duplicates=True):
        streams = att_streams(load(path))
        total = sum(len(v) for v in streams.values())
        consumed = n = 0
        for buf in streams.values():
            found = frames(buf)
            n += len(found)
            consumed += sum(len(f) for f in found)
        pct = 100.0 * consumed / total if total else 0.0
        tag = " (duplicate)" if name in DUPLICATES else ""
        print(f"{name:42s} bytes={total:7d} frames={n:5d} coverage={pct:5.1f}%{tag}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["sets", "gets", "acks", "order", "coverage"])
    parser.add_argument(
        "--dir", default="resources/traffic_capture", help="capture directory (default: resources/traffic_capture)"
    )
    args = parser.parse_args()
    if not os.path.isdir(args.dir):
        sys.exit(f"no such directory: {args.dir}")
    {"sets": cmd_sets, "gets": cmd_gets, "acks": cmd_acks, "order": cmd_order, "coverage": cmd_coverage}[args.command](
        args
    )


if __name__ == "__main__":
    main()
