#!/usr/bin/env python3
"""Extract a retail PS3 PSN .pkg (type 1). Usage: pkg_extract.py <in.pkg> <outdir>"""
import os, struct, sys
from Crypto.Cipher import AES

PS3_PKG_KEY = bytes.fromhex("2E7B71D7C9C9A14EA3221F188828B8F8")


class PkgReader:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0xC0)
        if h[:4] != b"\x7fPKG":
            raise ValueError("not a PKG")
        self.rev, self.type = struct.unpack(">HH", h[4:8])
        self.item_count, = struct.unpack(">I", h[0x14:0x18])
        self.total_size, self.data_off, self.data_size = struct.unpack(">QQQ", h[0x18:0x30])
        self.content_id = h[0x30:0x60].rstrip(b"\0").decode()
        self.riv = int.from_bytes(h[0x70:0x80], "big")
        self.ecb = AES.new(PS3_PKG_KEY, AES.MODE_ECB)

    def read(self, rel_off, size):
        """Read `size` decrypted bytes at offset `rel_off` relative to data start."""
        start = rel_off & ~0xF
        pad = rel_off - start
        n = (pad + size + 15) & ~0xF
        self.f.seek(self.data_off + start)
        enc = self.f.read(n)
        block = start // 16
        ctr = b"".join(((self.riv + block + i) & ((1 << 128) - 1)).to_bytes(16, "big")
                       for i in range(len(enc) // 16))
        ks = self.ecb.encrypt(ctr)
        dec = bytes(a ^ b for a, b in zip(enc, ks)) if len(enc) < 4096 else \
            (int.from_bytes(enc, "big") ^ int.from_bytes(ks[:len(enc)], "big")).to_bytes(len(enc), "big")
        return dec[pad:pad + size]

    def items(self):
        tbl = self.read(0, self.item_count * 32)
        for i in range(self.item_count):
            no, ns, do, ds, flags, _ = struct.unpack(">IIQQII", tbl[i * 32:(i + 1) * 32])
            name = self.read(no, ns).decode("utf-8", "replace")
            yield name, do, ds, flags


def main():
    src, out = sys.argv[1], sys.argv[2]
    p = PkgReader(src)
    print(f"{p.content_id}  items={p.item_count}  data={p.data_size:#x}")
    root = os.path.join(out, p.content_id.split("-")[1][:9])
    for name, do, ds, flags in p.items():
        kind = flags & 0xFF
        dest = os.path.join(root, name)
        if kind == 4:  # directory
            os.makedirs(dest, exist_ok=True)
            print(f"  [dir ] {name}")
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as o:
            chunk = 16 << 20
            pos = 0
            while pos < ds:
                n = min(chunk, ds - pos)
                o.write(p.read(do + pos, n))
                pos += n
        print(f"  [{kind:#04x}] {name}  {ds:,}")


if __name__ == "__main__":
    main()
