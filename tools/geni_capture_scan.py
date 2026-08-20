#!/usr/bin/env python3
"""Reassembling GENIbus scanner for resources/traffic_capture.

Why this exists. The corpus README documents a trap that has cost this project
hours twice: the negotiated MTU leaves 20 bytes of ATT payload, reads are
11-byte telegrams and fit in one packet, and **almost every write is longer and
is split across several**. A scanner that looks for CRC-valid frames inside
individual packets finds every read and 13 of the 420 writes, so the corpus
reads as a pump nobody writes to. That conclusion was reached twice, once
confidently enough to reach code comments, a changelog and a GitHub issue.

So this reassembles before it scans: HCI ACL -> L2CAP CID 0x0004 -> ATT opcode +
handle -> value, concatenated per (acl_handle, att_handle, direction). Note
which layer is load-bearing. The app does not send one long ATT PDU that the
controller fragments; it sends a sequence of independent ATT Write Commands to
the same handle, so it is the **ATT-value concatenation** that recovers a write,
not the HCI PB flag. PB continuation is handled too, because the corpus contains
some, but honouring it alone would not have found a single extra write. Anyone
re-deriving this from "fragmentation" alone fixes the wrong layer.

The reassembly is self-checking: `coverage` reports the fraction of each
GENIbus-carrying stream consumed by CRC-valid frames, and a sound parse is
exactly 100% with nothing left over.

It also drops the duplicate files: four are the same session as another and two
carry no ACL data at all. Counting every file gives 479 Class 10 SETs instead of
420.

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
    "10.31.25 2.29.26\u202fPM.pklg",  # no ACL data
}

# Three of the date-named captures separate the time from AM/PM with U+202F, a
# narrow no-break space, not U+0020. An entry above written with an ordinary
# space silently never matches -- which is what happened, and it is invisible
# because the two render identically.

CAPTURE_EXTENSIONS = (".pklg", ".log", ".pcap")


def records_btsnoop(blob):
    """Yield (is_receive, microseconds, acl_bytes) from a btsnoop file (H1)."""
    off = 16  # 'btsnoop\0' + version(4) + datalink(4)
    while off + 24 <= len(blob):
        _olen, ilen, flags, _drops, ts = struct.unpack_from(">IIIIq", blob, off)
        off += 24
        data = blob[off : off + ilen]
        off += ilen
        if flags & 0x02:  # command/event, not ACL data
            continue
        yield bool(flags & 0x01), ts, data


def _walk_pklg(blob, fmt):
    """Walk a PacketLogger file with one endianness. -> (records, bytes consumed).

    Records are (kind, data). The caller picks the endianness by which walk
    lands exactly on EOF; see records_pklg().
    """
    out, off, end = [], 0, len(blob)
    while off + 13 <= end:
        length = struct.unpack_from(fmt, blob, off)[0]
        if not 9 <= length <= 70000:
            break
        sec, usec = struct.unpack_from(fmt + fmt[-1], blob, off + 4)
        out.append((blob[off + 12], sec * 1000000 + usec, blob[off + 13 : off + 4 + length]))
        off += 4 + length
    return out, off


def records_pklg(blob):
    """(is_receive, microseconds, acl_bytes) for each ACL record in a PacketLogger file.

    The corpus holds both endiannesses, and the length field is the only thing
    that distinguishes them. Deciding it per record from the value's plausible
    range does NOT work: a little-endian length of 0x0100 is `00 01 00 00`,
    which reads big-endian as 65536 -- inside any sane range -- so the walk
    jumps 64 KB and silently abandons the rest of the file. That cost 40% of
    `grundfos_authentication.pklg` and overran the end of two others.

    Decide it once per file instead, on a criterion with no judgement in it: a
    correct walk consumes the file exactly. Both are tried and the one that
    lands on EOF wins; if neither does, the one that got furthest without
    overrunning is used, which keeps a truncated capture partially readable
    rather than empty.
    """
    exact, partial = [], []
    for fmt in (">I", "<I"):
        records, consumed = _walk_pklg(blob, fmt)
        # A zero-record walk "lands on EOF" for an empty file in both
        # directions; requiring records is what keeps that from being a
        # coin flip.
        (exact if consumed == len(blob) and records else partial).append((fmt, records))
    if len(exact) > 1:
        raise ValueError("ambiguous PacketLogger endianness: both walks consume the file")
    if not exact:
        # Nothing consumed the file cleanly. Take the longer walk so a truncated
        # capture is still partly readable, but do not pretend it was clean.
        best = max(partial, key=lambda c: len(c[1]), default=(None, []))
        if not best[1]:
            raise ValueError("not a readable PacketLogger file at either endianness")
        records = best[1]
    else:
        records = exact[0][1]
    # Eager, not a generator: the validation above has to fail at the call, not
    # at first iteration, or an unreadable file looks like an empty one.
    return [
        (kind == 0x03, ts, data)  # 0x02 ACL out (host -> controller), 0x03 ACL in
        for kind, ts, data in records
        if kind in (0x02, 0x03)
    ]


def load_records(path):
    blob = open(path, "rb").read()
    if blob[:8] == b"btsnoop\x00":
        return list(records_btsnoop(blob))
    return list(records_pklg(blob))


# Historical name; att_streams() takes records, not a path.
load = load_records


def att_streams(records):
    """Concatenated ATT values, keyed (acl_handle, att_handle, is_receive)."""
    partial, streams = {}, {}
    for recv, _ts, data in records:
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


def timed_frames(path):
    """Chronological (microseconds, is_receive, frame) for one capture.

    att_streams() concatenates per stream and loses ordering, which is right for
    a census and useless for latency. This keeps the record timestamp of the ATT
    PDU that COMPLETED each frame -- the last fragment, i.e. the moment the frame
    was fully on the wire -- and interleaves the directions.
    """
    partial, buffers, out = {}, {}, []
    for recv, ts, data in load_records(path):
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
            continue
        pdu = bytes(buf[4 : 4 + l2len])
        del partial[key]
        if cid != 0x0004 or len(pdu) < 3 or pdu[0] not in (0x12, 0x52, 0x1B, 0x1D):
            continue
        att_handle = struct.unpack_from("<H", pdu, 1)[0]
        skey = (key[0], att_handle, recv)
        stream = buffers.setdefault(skey, bytearray())
        stream.extend(pdu[3:])
        # Drain whatever complete frames this fragment finished.
        while len(stream) >= 2:
            if stream[0] not in (0x24, 0x27):
                del stream[0]
                continue
            total_len = stream[1] + 4
            if len(stream) < total_len:
                break
            frame = bytes(stream[:total_len])
            if crc16(frame[1:-2]) == struct.unpack(">H", frame[-2:])[0]:
                out.append((ts, recv, frame))
                del stream[:total_len]
            else:
                del stream[0]
    out.sort(key=lambda r: r[0])
    return out


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


def cmd_latency(args):
    """Request/reply latency, paired chronologically within each capture.

    The pairing rule is the protocol's own: GENIbus is interlocked, one request
    outstanding at a time (App. Prog. Manual fig 1), so a reply belongs to the
    most recent unanswered request. Anything over `--gap` ms is dropped as an
    idle-gap artifact rather than a slow answer -- an unanswered request paired
    against a much later one.
    """

    def pct(xs, q):
        return sorted(xs)[min(len(xs) - 1, int(q * len(xs)))]

    per_shape, all_dt, dropped = {}, [], 0
    for _name, path in captures(args.dir):
        pending = None
        for ts, recv, frame in timed_frames(path):
            apdu = frame[4:-2]
            if len(apdu) < 2 or apdu[0] != 0x0A:
                continue
            if not recv:
                op = (apdu[1] >> 6) & 3
                body = apdu[2 : 2 + (apdu[1] & 0x3F)]
                shape = None
                if len(body) >= 3:
                    obj, sub, _t, _v = address(body)
                    shape = ("SET" if op == 2 else "GET", obj, sub)
                pending = (ts, shape)
                continue
            if pending is None:
                continue
            dt = (ts - pending[0]) / 1000.0
            shape = pending[1]
            pending = None
            if dt < 0 or dt > args.gap:
                dropped += 1
                continue
            all_dt.append(dt)
            if shape:
                per_shape.setdefault(shape, []).append(dt)
    if not all_dt:
        print("no pairs")
        return
    print(f"{len(all_dt)} pairs ({dropped} dropped over {args.gap} ms)")
    print(
        f"  all:  min {min(all_dt):.0f}  p50 {pct(all_dt, 0.50):.0f}  "
        f"p90 {pct(all_dt, 0.90):.0f}  p99 {pct(all_dt, 0.99):.0f}  max {max(all_dt):.0f} ms"
    )
    print(f"\n{'op':>4} {'obj':>4} {'sub':>5} {'n':>5}  min-max ms   p50")
    for shape in sorted(per_shape, key=lambda k: (k[1], k[2])):
        xs = per_shape[shape]
        op, obj, sub = shape
        print(f"{op:>4} {obj:4d} {sub:5d} {len(xs):5d}  {min(xs):.0f}-{max(xs):.0f}".ljust(38) + f"{pct(xs, 0.50):.0f}")


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
    """Reassembly soundness, per file.

    The denominator is only the streams that carry GENIbus at all. A connection
    also carries CCCD writes and other GATT traffic, and including those puts a
    few hundred bytes of legitimate non-GENIbus noise in the denominator -- which
    both understates a clean parse and, worse, leaves room for a real mid-stream
    gap to hide inside the shortfall. Restricted properly, a sound parse is
    exactly 100.0%, and anything less is a defect rather than noise.
    """
    for name, path in captures(args.dir, include_duplicates=True):
        streams = att_streams(load(path))
        total = consumed = n = skipped = 0
        for buf in streams.values():
            found = frames(buf)
            if not found:
                skipped += len(buf)  # not a GENIbus stream
                continue
            n += len(found)
            total += len(buf)
            consumed += sum(len(f) for f in found)
        pct = 100.0 * consumed / total if total else 0.0
        tag = " (duplicate)" if name in DUPLICATES else ""
        note = f"  [{skipped} non-GENIbus bytes]" if skipped else ""
        print(f"{name:42s} bytes={total:7d} frames={n:5d} coverage={pct:6.2f}%{tag}{note}")


def cmd_selftest(args):
    """Assertions about the scanner itself, plus a corpus check when one is here.

    This exists because the tool shipped with a defect that discarded 40% of one
    capture in silence, and the census it produced was right only because the
    damaged file happened to be de-duplicated out. Review did not catch that; a
    parse that must land on EOF does.

    The pure-function half needs no captures and can run anywhere. The corpus
    half is skipped when `resources/` is absent, which it is for anyone who did
    not do the reverse engineering -- `resources/` is gitignored.
    """
    fails = []

    def check(ok, what):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}")
        if not ok:
            fails.append(what)

    print("pure functions:")
    # The canonical short acknowledgement, and the tie to codec.cpp's CRC.
    check(crc16(bytes.fromhex("05f8e70a0100")) == 0xAEA2, "crc16 matches the captured short ACK")
    ack = bytes.fromhex("2405f8e70a0100aea2")
    check([f for f in frames(bytearray(ack))] == [ack], "one whole frame parses")
    check(frames(bytearray(b"")) == [], "empty input yields nothing")
    check(frames(bytearray(ack[:5])) == [], "a truncated frame yields nothing")
    check(frames(bytearray(b"\x00\x99" + ack)) == [ack], "junk before a frame is skipped")
    check(frames(bytearray(ack + ack)) == [ack, ack], "back-to-back frames both parse")
    bad = bytearray(ack)
    bad[-1] ^= 0xFF
    check(frames(bad) == [], "a bad CRC is rejected")
    check(frames(bytearray(bytes(bad) + ack)) == [ack], "...and the walker resynchronises past it")

    # PacketLogger endianness, on synthetic records rather than on the corpus.
    def pklg(fmt, kind, body):
        n = 9 + len(body)
        return struct.pack(fmt, n) + struct.pack(fmt, 1) + struct.pack(fmt, 2) + bytes([kind]) + body

    for fmt, label in ((">I", "big-endian"), ("<I", "little-endian")):
        blob = pklg(fmt, 0x02, b"\x01\x02\x03\x04") + pklg(fmt, 0x03, b"\x05\x06")
        got = list(records_pklg(blob))
        check(got == [(False, 1000002, b"\x01\x02\x03\x04"), (True, 1000002, b"\x05\x06")], f"{label} records parse")
    # The defect this test exists for: a length whose LE bytes read as a
    # plausible BE length. 0x0100 little-endian is `00 01 00 00`, 65536 big.
    body = b"\x00" * (0x0100 - 9)
    blob = pklg("<I", 0x02, body) + pklg("<I", 0x02, b"\x07\x08")
    got = list(records_pklg(blob))
    check(len(got) == 2 and got[1][2] == b"\x07\x08", "a 0x0100-length little-endian record does not derail the walk")
    try:
        records_pklg(b"not a packetlogger file at all")
        raised = False
    except ValueError:
        raised = True
    check(raised, "an unreadable file raises rather than returning nothing")

    if not os.path.isdir(args.dir):
        print(f"\ncorpus checks: SKIPPED, no {args.dir}")
    else:
        print(f"\ncorpus ({args.dir}):")
        present = {os.path.basename(p) for p in glob.glob(os.path.join(args.dir, "*"))}
        missing = sorted(DUPLICATES - present)
        check(not missing, f"every DUPLICATES entry names a real file{'' if not missing else f': {missing}'}")
        worst = 100.0
        for _name, path in captures(args.dir, include_duplicates=True):
            for buf in att_streams(load(path)).values():
                found = frames(buf)
                if found:
                    worst = min(worst, 100.0 * sum(len(f) for f in found) / len(buf))
        check(worst == 100.0, f"every GENIbus stream is consumed entirely (worst {worst:.2f}%)")
        n_sets = shapes = 0
        census = {}
        for _name, recv, apdu in apdus(args.dir):
            if recv or len(apdu) < 5 or apdu[0] != 0x0A or (apdu[1] >> 6) != 2:
                continue
            n_sets += 1
            census[address(apdu[2 : 2 + (apdu[1] & 0x3F)])[:2]] = 1
        shapes = len(census)
        check(n_sets == 420, f"420 Class 10 SETs (got {n_sets})")
        check(shapes == 20, f"20 distinct address shapes (got {shapes})")
        acks = {}
        for _name, recv, apdu in apdus(args.dir):
            if not recv or len(apdu) < 3 or apdu[0] != 0x0A or (apdu[1] & 0x3F) > 2:
                continue
            acks[apdu[2]] = acks.get(apdu[2], 0) + 1
        check(acks == {0: 420, 2: 26, 4: 13}, f"459 short replies, 420/26/13 (got {acks})")

    print("\nFAILED" if fails else "\nok")
    if fails:
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["sets", "gets", "acks", "latency", "order", "coverage", "selftest"])
    parser.add_argument(
        "--dir", default="resources/traffic_capture", help="capture directory (default: resources/traffic_capture)"
    )
    parser.add_argument(
        "--gap", type=float, default=1000.0, help="latency: drop pairs slower than this, in ms (default 1000)"
    )
    args = parser.parse_args()
    if args.command != "selftest" and not os.path.isdir(args.dir):
        sys.exit(f"no such directory: {args.dir}")
    {
        "sets": cmd_sets,
        "gets": cmd_gets,
        "acks": cmd_acks,
        "latency": cmd_latency,
        "order": cmd_order,
        "coverage": cmd_coverage,
        "selftest": cmd_selftest,
    }[args.command](args)


if __name__ == "__main__":
    main()
