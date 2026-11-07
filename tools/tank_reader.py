"""Bounded streaming form of extract_active_heroes.py's CRC-verified reader.

RAW and existing zlib/chunk-tail algorithm only; unknown codecs fail closed.
No writer, native code, external executable or game process.
"""
from pathlib import Path, PurePosixPath
import struct
import zlib


def safe_internal(name):
    name = name.replace("\\", "/")
    p = PurePosixPath(name)
    if (not name or p.is_absolute() or any(x in ("", ".", "..") for x in name.split("/"))
            or any(c in name for c in ':\x00<>"|?*')):
        raise ValueError(f"unsafe Tank path: {name!r}")
    if any(x.endswith((".", " ")) or x.split('.')[0].upper() in
           {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(1,10)], *[f"LPT{i}" for i in range(1,10)]}
           for x in p.parts):
        raise ValueError(f"unsafe Windows path: {name!r}")
    return p.as_posix()


class TankReader:
    def __init__(self, path):
        self.path = Path(path)
        self.file = self.path.open("rb")
        self.size = self.path.stat().st_size
        try:
            if self.read(0, 8) != b"DSigTank":
                raise ValueError("not a DS1 Tank")
            self.ds, self.fs, _, self.data = self.unpack("<4I", 12)
            if not (28 <= self.ds < self.size and 28 <= self.fs < self.size and self.data <= self.size):
                raise ValueError("Tank offsets outside file")
        except BaseException:
            self.file.close()
            raise

    def __enter__(self): return self
    def __exit__(self, *args): self.file.close()

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError("Tank read outside file")
        self.file.seek(offset)
        b = self.file.read(size)
        if len(b) != size: raise ValueError("truncated Tank")
        return b

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.read(offset, struct.calcsize(fmt)))

    def entries(self):
        count, = self.unpack("<I", self.fs)
        if count > 1_000_000 or self.fs + 4 + count * 4 > self.size:
            raise ValueError("invalid Tank file count")
        seen = set()
        for i in range(count):
            e = self.fs + self.unpack("<I", self.fs + 4 + 4*i)[0]
            parent, size, offset, crc = self.unpack("<4I", e)
            comp, = self.unpack("<H", e+24)
            ln, = self.unpack("<H", e+28)
            parts = [self.read(e+30, ln).decode("ascii")]
            parents = set()
            while parent:
                if parent in parents or len(parents) > 256: raise ValueError("Tank directory cycle")
                parents.add(parent)
                p = self.ds + parent
                n, = self.unpack("<H", p+16)
                if n: parts.insert(0, self.read(p+18, n).decode("ascii"))
                parent, = self.unpack("<I", p)
            name = safe_internal('/'.join(parts))
            if name.casefold() in seen: raise ValueError("case-insensitive Tank path collision")
            seen.add(name.casefold())
            yield dict(internal_path=name, size=size, offset=self.data+offset, crc32=crc,
                       compression=comp, compressed_header=e+28+((ln+6)&~3))

    def chunks(self, entry):
        size, start, comp = entry['size'], entry['offset'], entry['compression']
        crc, total = 0, 0
        if comp == 0:
            def raw():
                for n in range(0, size, 1024*1024):
                    yield self.read(start+n, min(1024*1024, size-n))
            source = raw()
        elif comp == 1:
            def packed():
                header = entry['compressed_header']
                packed_size, chunk_size = self.unpack('<2I', header)
                if chunk_size:
                    if chunk_size > 16*1024*1024: raise ValueError('oversized compressed chunk')
                    for i in range((size+chunk_size-1)//chunk_size):
                        unpacked, encoded_size, extra, offset = self.unpack('<4I', header+8+16*i)
                        if unpacked > chunk_size or extra > unpacked or encoded_size > 32*1024*1024:
                            raise ValueError('invalid compressed chunk sizes')
                        encoded = self.read(start+offset, encoded_size)
                        if encoded_size == unpacked:
                            decoded = encoded
                        else:
                            decoder = zlib.decompressobj()
                            decoded = decoder.decompress(encoded, unpacked+1)
                            if not decoder.eof or decoder.unconsumed_tail or len(decoded) != unpacked-extra:
                                raise ValueError('invalid zlib chunk')
                        yield decoded
                        if extra: yield self.read(start+offset+encoded_size, extra)
                else:
                    decoder = zlib.decompressobj()
                    remaining = size
                    for n in range(0, packed_size, 65536):
                        encoded = self.read(start+n, min(65536, packed_size-n))
                        while encoded:
                            decoded = decoder.decompress(encoded, min(remaining+1, 1024*1024))
                            encoded = decoder.unconsumed_tail
                            if len(decoded)>remaining: raise ValueError('zlib output exceeds Tank size')
                            remaining -= len(decoded)
                            yield decoded
                    if not decoder.eof: raise ValueError('truncated zlib stream')
            source = packed()
        else:
            raise ValueError(f'unsupported Tank compression: {comp}')
        for block in source:
            total += len(block)
            if total > size: raise ValueError('decoded Tank size exceeded')
            crc = zlib.crc32(block, crc)
            yield block
        if total != size or crc != entry['crc32']:
            raise ValueError(f"Tank CRC/size mismatch: {entry['internal_path']}")

    def payload(self, entry, limit=16*1024*1024):
        if entry['size'] > limit: raise ValueError('metadata exceeds read bound')
        return b''.join(self.chunks(entry))
