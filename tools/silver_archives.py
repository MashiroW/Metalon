"""
Readers for the original Silver (1999) CD data: the data*.nob / fat*.nob
container, RNC-1 decompression and the level .PAK layout.

The RNC decompressor and the PAK layout are ports of dernc.c / pak.c from
the "silvie" Silver asset extractor by Lucas Petitiot
(https://github.com/othias/silvie, GPL-3.0); RNC compression by Rob
Northen. This file is therefore distributed under GPL-3.0.
"""
import struct
from pathlib import Path

# ---------------------------------------------------------------- nob / fat
# fat*.nob: u32 count, then count records of 136 bytes:
#   char path[128] (original build path, e.g. "y:\silver\levels\GNO\...")
#   u32 offset, u32 size   -> bytes in the matching data*.nob
RECORD_SIZE = 136
PATH_FIELD_SIZE = 128


class NobEntry:
    __slots__ = ("path", "offset", "size")

    def __init__(self, path, offset, size):
        self.path, self.offset, self.size = path, offset, size

    @property
    def rel_path(self):
        """Path under the original 'silver\\' root, e.g. levels\\GNO\\BOILAREA\\BOILAREA.PAK"""
        idx = self.path.lower().find(b"\\silver\\")
        if idx == -1:
            raise ValueError(f"unexpected path shape: {self.path!r}")
        return self.path[idx + 8:].decode("latin1")


def parse_fat(fat_path):
    data = Path(fat_path).read_bytes()
    count = struct.unpack_from("<I", data, 0)[0]
    if 4 + count * RECORD_SIZE != len(data):
        raise ValueError(f"{fat_path}: unexpected size")
    entries = []
    for i in range(count):
        rec = data[4 + i * RECORD_SIZE: 4 + (i + 1) * RECORD_SIZE]
        path = rec[:PATH_FIELD_SIZE].split(b"\x00", 1)[0]
        offset, size = struct.unpack_from("<II", rec, PATH_FIELD_SIZE)
        entries.append(NobEntry(path, offset, size))
    return entries


def extract_entry(data_path, entry):
    with open(data_path, "rb") as f:
        f.seek(entry.offset)
        return f.read(entry.size)


def find_archives(game_dirs):
    """(fat, data) pairs found in the given folders (CD1 / CD2 'SILVER' folders)."""
    pairs = []
    for d in game_dirs:
        for fat in sorted(Path(d).glob("fat*.nob")):
            data = fat.with_name("data" + fat.name[3:])
            if data.exists():
                pairs.append((fat, data))
    return pairs


def room_paks(game_dirs):
    """{'level/room': (data_path, entry)} for every levels\\<LEVEL>\\<ROOM>\\<ROOM>.PAK"""
    rooms = {}
    for fat, data in find_archives(game_dirs):
        for e in parse_fat(fat):
            parts = e.rel_path.split("\\")
            if len(parts) == 4 and parts[0].lower() == "levels" and parts[3].upper() == parts[2].upper() + ".PAK":
                rooms[f"{parts[1].lower()}/{parts[2].lower()}"] = (data, e)
    return rooms


# ---------------------------------------------------------------- RNC-1
class RncError(ValueError):
    pass


def _crc_table():
    tab = []
    for i in range(256):
        v = i
        for _ in range(8):
            v = (v >> 1) ^ 0xA001 if (v & 1) else (v >> 1)
        tab.append(v)
    return tab


_CRC = _crc_table()


def rnc_crc(data):
    v = 0
    for b in data:
        v ^= b
        v = (v >> 8) ^ _CRC[v & 0xFF]
    return v & 0xFFFF


class _Bits:
    def __init__(self, buf, pos):
        self.buf, self.pos = buf, pos
        self.bitbuf = self._word()
        self.bitcount = 16

    def _word(self):
        return self.buf[self.pos] | (self.buf[self.pos + 1] << 8)

    def fix(self):
        self.bitcount -= 16
        self.bitbuf &= (1 << self.bitcount) - 1
        self.bitbuf |= self._word() << self.bitcount
        self.bitcount += 16

    def advance(self, n):
        self.bitbuf >>= n
        self.bitcount -= n
        if self.bitcount < 16:
            self.pos += 2
            self.bitbuf |= self._word() << self.bitcount
            self.bitcount += 16

    def read(self, mask, n):
        r = self.bitbuf & mask
        self.advance(n)
        return r


def _mirror(x, n):
    top, bottom = 1 << (n - 1), 1
    while top > bottom:
        mask = top | bottom
        m = x & mask
        if m != 0 and m != mask:
            x ^= mask
        top >>= 1
        bottom <<= 1
    return x


def _read_huftable(bs):
    num = bs.read(0x1F, 5)
    if num == 0:
        return []
    lens = [bs.read(0x0F, 4) for _ in range(num)]
    table, code = [], 0
    for length in range(1, max(lens) + 1):
        for j in range(num):
            if lens[j] == length:
                table.append((_mirror(code, length), length, j))
                code += 1
        code <<= 1
    return table


def _huf_read(table, bs):
    for code, cl, value in table:
        if bs.bitbuf & ((1 << cl) - 1) == code:
            bs.advance(cl)
            if value >= 2:
                base = 1 << (value - 1)
                value = base | bs.read(base - 1, value - 1)
            return value
    raise RncError("huffman decode error")


def rnc_unpack(packed):
    if len(packed) < 18 or packed[0:3] != b"RNC" or packed[3] != 1:
        raise RncError("not an RNC-1 file")
    unpacked_sz, packed_sz = struct.unpack_from(">II", packed, 4)
    unpacked_crc, packed_crc = struct.unpack_from(">HH", packed, 12)
    if rnc_crc(packed[18:18 + packed_sz]) != packed_crc:
        raise RncError("packed CRC mismatch")
    packed = packed + b"\x00" * 8
    out = bytearray(unpacked_sz + 8)
    o = 0
    bs = _Bits(packed, 18)
    bs.advance(2)
    while o < unpacked_sz:
        raw_t, dist_t, len_t = _read_huftable(bs), _read_huftable(bs), _read_huftable(bs)
        count = bs.read(0xFFFF, 16)
        while True:
            n = _huf_read(raw_t, bs)
            if n:
                out[o:o + n] = packed[bs.pos:bs.pos + n]
                o += n
                bs.pos += n
                bs.fix()
            count -= 1
            if count <= 0:
                break
            posn = _huf_read(dist_t, bs) + 1
            n = _huf_read(len_t, bs) + 2
            for _ in range(n):
                out[o] = out[o - posn]
                o += 1
    result = bytes(out[:unpacked_sz])
    if rnc_crc(result) != unpacked_crc:
        raise RncError("unpacked CRC mismatch")
    return result


# ---------------------------------------------------------------- level .PAK
def parse_pak(unpacked):
    """Sections of a decompressed level .PAK: background (44-byte header,
    768-byte palette, pixels), out_0, out_1, out_2 (112-byte camera header
    + body)."""
    pos = 0

    def u32():
        nonlocal pos
        v = struct.unpack_from("<I", unpacked, pos)[0]
        pos += 4
        return v

    u32(); hdr = unpacked[pos:pos + 44]; pos += 44
    u32(); pal = unpacked[pos:pos + 768]; pos += 768
    n = u32(); pixels = unpacked[pos:pos + n]; pos += n
    n = u32(); out_0 = unpacked[pos:pos + n]; pos += n
    n = u32(); out_1 = unpacked[pos:pos + n]; pos += n
    u32(); out_2_hdr = unpacked[pos:pos + 112]; pos += 112
    n = u32(); out_2_buf = unpacked[pos:pos + n]; pos += n
    return {"background": hdr + pal + pixels, "out_0": out_0, "out_1": out_1, "out_2": out_2_hdr + out_2_buf}
