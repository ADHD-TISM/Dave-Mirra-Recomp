"""Extract the ISO9660 data track from a MODE2/2352 PS1 BIN image.

usage: python extract_iso.py <image.bin> <out_dir>
Writes every file in the filesystem tree to out_dir, plus files.txt listing
name, LBA and size (needed later to serve raw CD reads / XA streams).
Names that would land outside out_dir are skipped.
"""
import os, struct, sys

SECTOR = 2352
DATA_OFF = 24  # sync(12) + header(4) + subheader(8) for Mode 2 Form 1
MAX_DEPTH = 16

def read_sector(f, lba):
    f.seek(lba * SECTOR + DATA_OFF)
    return f.read(2048)

def read_extent(f, lba, size):
    out = bytearray()
    n = (size + 2047) // 2048
    for i in range(n):
        out += read_sector(f, lba + i)
    return bytes(out[:size])

def safe_name(name):
    return bool(name) and not any(c in name for c in '/\\:\0') and not name.startswith('.')

def walk(f, lba, size, path, listing, out_dir, depth=0):
    if depth > MAX_DEPTH or size > 64 * 1024 * 1024:
        return
    root = os.path.realpath(out_dir) + os.sep
    data = read_extent(f, lba, size)
    pos = 0
    while pos + 34 <= len(data):
        ln = data[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + ln]
        pos += ln
        if len(rec) < 34:
            break
        ext = struct.unpack_from('<I', rec, 2)[0]
        sz = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0]
        if not safe_name(name):
            print(f'skipping unsafe name {name!r}', file=sys.stderr)
            continue
        full = f'{path}/{name}' if path else name
        dst = os.path.realpath(os.path.join(out_dir, full))
        if not dst.startswith(root):
            print(f'skipping {full!r}: outside the output folder', file=sys.stderr)
            continue
        if flags & 2:
            os.makedirs(dst, exist_ok=True)
            walk(f, ext, sz, full, listing, out_dir, depth + 1)
        else:
            listing.append((full, ext, sz))
            # XA/STR streams are interleaved Form 2; extract the 2048-byte view
            # anyway, the runtime reads raw sectors via files.txt.
            with open(dst, 'wb') as o:
                o.write(read_extent(f, ext, sz))

def main():
    img, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with open(img, 'rb') as f:
        pvd = read_sector(f, 16)
        assert pvd[1:6] == b'CD001', 'no ISO9660 primary volume descriptor'
        root = pvd[156:156 + 34]
        lba = struct.unpack_from('<I', root, 2)[0]
        sz = struct.unpack_from('<I', root, 10)[0]
        listing = []
        walk(f, lba, sz, '', listing, out_dir)
    with open(os.path.join(out_dir, 'files.txt'), 'w') as o:
        for n, l, s in listing:
            o.write(f'{n}\t{l}\t{s}\n')
    for n, l, s in listing:
        print(f'{l:8d} {s:10d}  {n}')

if __name__ == '__main__':
    main()
