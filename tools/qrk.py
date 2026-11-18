#!/usr/bin/env python3
"""Fat Princess / Quark engine .qrk archive tool (little-endian).

Layout:
  Header (24 bytes): char magic[4]='QRK\0', u32 version(=1), u32 count, u32 dataStart (absolute),
                     u32 unk1(=1), u32 namesSize
  Names block: namesSize bytes of NUL-terminated paths
  TOC: count x 21-byte entries
       u32 storedSize, u32 offset (absolute), u32 rawSize, u32 hash, u32 nameOffset, u8 zlibFlag
  Data: flag=1 -> zlib stream (78 9C) inflating to rawSize; flag=0 -> stored as-is.

Usage:
  qrk.py list    <archive.qrk>
  qrk.py extract <archive.qrk> <outdir>
  qrk.py pack    <indir> <archive.qrk>     (rebuild, for mods; see pack())
"""
import os, struct, sys, zlib


class Qrk:
    def __init__(s, path):
        s.f = open(path, 'rb')
        h = s.f.read(24)
        s.unk0, s.ver, s.count, s.datastart, s.unk1, s.nsz = struct.unpack('<6I', h)
        names = s.f.read(s.nsz)
        toc = s.f.read(21 * s.count)
        s.entries = []
        for i in range(s.count):
            cs, off, rs, hs, no, fl = struct.unpack('<5IB', toc[i * 21:i * 21 + 21])
            nm = names[no:names.index(b'\0', no)].decode()
            s.entries.append(dict(name=nm, csize=cs, off=off, size=rs, hash=hs, comp=fl, nameoff=no))

    def raw(s, e):
        s.f.seek(e['off'])
        return s.f.read(e['csize'])

    def data(s, e):
        b = s.raw(e)
        if e['comp']:
            b = zlib.decompress(b)
        if len(b) != e['size']:
            raise ValueError(f"size mismatch {e['name']}: {len(b)} != {e['size']}")
        return b


def pack(indir, out, template=None):
    """Rebuild an archive from a directory. Entry order and hash/unk fields are copied
    from `template` (the original archive) when given, so a repack is as close to the
    original as possible. New files get hash 0 until the hash function is identified (hash is NOT crc32/adler32 of the data)."""
    tmpl = {e['name']: e for e in Qrk(template).entries} if template else {}
    order = [e['name'] for e in Qrk(template).entries] if template else []
    for root, _, files in os.walk(indir):
        for fn in files:
            rel = os.path.relpath(os.path.join(root, fn), indir).replace(os.sep, '/')
            if rel not in tmpl:
                order.append(rel)
    names = b''; noffs = {}
    for n in order:
        noffs[n] = len(names); names += n.encode() + b'\0'
    datastart = 24 + len(names) + 21 * len(order)
    blobs = []; toc = b''; pos = datastart
    for n in order:
        raw = open(os.path.join(indir, n), 'rb').read()
        t = tmpl.get(n)
        comp = t['comp'] if t else 1
        stored = zlib.compress(raw, 9) if comp else raw
        toc += struct.pack('<5IB', len(stored), pos, len(raw), t['hash'] if t else 0, noffs[n], comp)
        blobs.append(stored); pos += len(stored)
    with open(out, 'wb') as o:
        o.write(struct.pack('<6I', 0x004B5251, 1, len(order), datastart, 1, len(names)))
        o.write(names); o.write(toc)
        for b in blobs: o.write(b)


def main():
    cmd = sys.argv[1]
    if cmd == 'list':
        q = Qrk(sys.argv[2])
        for e in q.entries:
            print(f"{e['size']:10d} {e['csize']:10d} {e['comp']} {e['hash']:08x} {e['name']}")
    elif cmd == 'extract':
        q = Qrk(sys.argv[2]); out = sys.argv[3]
        for e in q.entries:
            p = os.path.join(out, *e['name'].split('/'))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(q.data(e))
        print(f"extracted {q.count} files to {out}")
    elif cmd == 'pack':
        pack(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)


if __name__ == '__main__':
    main()
