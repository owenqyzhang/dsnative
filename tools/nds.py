"""Read a Nintendo DS ROM image (.nds): the header, the ARM9 and ARM7 binaries, the
Nitro SDK module parameters (autoload segments, BLZ compression), the overlay tables and
the file system (FNT/FAT).

Nothing here needs a particular game: every structure is found from the header and the
binaries themselves. References: GBATEK (DS Cartridge Header, DS Cartridge NitroROM
and NitroARC File Systems) and the Nitro SDK's module parameter layout.
"""
import struct
from dataclasses import dataclass, field

NITROCODE_BE = 0xDEC00621   # word order of the Nitro SDK module parameter signature
NITROCODE_LE = 0x2106C0DE


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


class RomError(Exception):
    pass


# ---------------------------------------------------------------------------
# BLZ: the backward LZ77 the Nitro SDK uses for the ARM9 static image and overlays.
# The compressed area is read from its end towards its start; a footer of 8 bytes at
# the end gives its sizes.
# ---------------------------------------------------------------------------

def blz_decompress(data):
    """Decompress a BLZ-compressed buffer and return the whole decompressed buffer."""
    data = bytes(data)
    n = len(data)
    if n < 8:
        raise RomError('BLZ: buffer shorter than its footer')
    inc_len = u32(data, n - 4)
    if inc_len == 0:
        return data[:n]
    hdr = u32(data, n - 8)
    hdr_len = hdr >> 24
    enc_len = hdr & 0xFFFFFF
    if hdr_len < 8 or hdr_len > 11 or enc_len > n or enc_len < hdr_len:
        raise RomError(f'BLZ: bad footer (enc_len {enc_len:#x}, hdr_len {hdr_len})')
    dec_len = n - enc_len           # the uncompressed part at the start
    pak_len = enc_len - hdr_len
    raw_len = dec_len + enc_len + inc_len
    out = bytearray(raw_len)
    out[:dec_len] = data[:dec_len]
    pak = dec_len + pak_len
    raw = raw_len
    while raw > dec_len and pak > dec_len:
        pak -= 1
        flags = data[pak]
        for _ in range(8):
            if raw <= dec_len or pak <= dec_len:
                break
            if flags & 0x80:
                pak -= 2
                v = (data[pak + 1] << 8) | data[pak]
                length = (v >> 12) + 3
                disp = (v & 0xFFF) + 3
                for _ in range(length):
                    if raw <= dec_len:
                        break
                    raw -= 1
                    if raw + disp >= raw_len:
                        raise RomError('BLZ: match outside the buffer')
                    out[raw] = out[raw + disp]
            else:
                pak -= 1
                raw -= 1
                out[raw] = data[pak]
            flags = (flags << 1) & 0xFF
    if raw != dec_len:
        raise RomError(f'BLZ: stream ended {raw - dec_len} bytes early')
    return bytes(out)


def blz_compress(data, keep=0):
    """Compress data with BLZ, leaving its first `keep` bytes uncompressed.

    Only used to build test images (a real game is compressed by the Nitro SDK); the
    output decompresses with blz_decompress and with the SDK's own routine."""
    data = bytes(data)
    rev = data[keep:][::-1]
    stream = bytearray()
    i = 0
    n = len(rev)
    while i < n:
        flag_pos = len(stream)
        stream.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len, best_disp = 0, 0
            lo = max(0, i - 0x1002)
            for j in range(i - 3, lo - 1, -1):
                if rev[j] != rev[i]:
                    continue
                k = 0
                while k < 18 and i + k < n and rev[j + k] == rev[i + k]:
                    k += 1
                if k > best_len:
                    best_len, best_disp = k, i - j
                    if k == 18:
                        break
            if best_len >= 3:
                v = ((best_len - 3) << 12) | (best_disp - 3)
                stream.append(v >> 8)
                stream.append(v & 0xFF)
                flags |= 0x80 >> bit
                i += best_len
            else:
                stream.append(rev[i])
                i += 1
        stream[flag_pos] = flags
    body = bytes(stream[::-1])
    pad = (-(keep + len(body))) % 4
    hdr_len = 8 + pad
    enc_len = len(body) + hdr_len
    total = keep + enc_len
    inc_len = len(data) - total
    if inc_len <= 0:
        return None     # compression would not shrink it
    return data[:keep] + body + b'\xff' * pad + struct.pack('<II', enc_len | (hdr_len << 24), inc_len)


# ---------------------------------------------------------------------------

@dataclass
class Segment:
    """A run of bytes the game has at a RAM address: the ARM9 static image, an autoload
    segment (ITCM, DTCM or another), an overlay or the ARM7 image."""
    name: str
    cpu: int            # 9 or 7
    addr: int
    data: bytes
    bss: int = 0
    kind: str = 'static'    # static, autoload, overlay
    overlay_id: int = -1

    @property
    def end(self):
        return self.addr + len(self.data)


@dataclass
class Overlay:
    id: int
    ram_addr: int
    ram_size: int
    bss_size: int
    sinit_start: int
    sinit_end: int
    file_id: int
    flags: int
    rom_start: int = 0
    rom_end: int = 0

    @property
    def compressed(self):
        return bool(self.flags & 0x01000000)

    @property
    def compressed_size(self):
        return self.flags & 0xFFFFFF


@dataclass
class ModuleParams:
    offset: int                 # in the ARM9 image
    autoload_list: int
    autoload_list_end: int
    autoload_start: int
    bss_start: int
    bss_end: int
    compressed_end: int
    sdk_version: int


@dataclass
class Rom:
    data: bytes
    title: str = ''
    gamecode: str = ''
    maker: str = ''
    unitcode: int = 0
    header: dict = field(default_factory=dict)
    arm9: bytes = b''           # decompressed
    arm7: bytes = b''
    arm9_compressed: bool = False
    params: ModuleParams = None
    autoloads: list = field(default_factory=list)   # list of Segment (ARM9)
    overlays9: list = field(default_factory=list)
    overlays7: list = field(default_factory=list)
    fat: list = field(default_factory=list)
    files: dict = field(default_factory=dict)       # path -> file id

    # -- loading ---------------------------------------------------------------
    @classmethod
    def load(cls, path_or_bytes):
        if isinstance(path_or_bytes, (bytes, bytearray)):
            data = path_or_bytes
        else:
            with open(path_or_bytes, 'rb') as f:
                data = f.read()
        rom = cls(bytes(data))
        rom._parse()
        return rom

    def _parse(self):
        d = self.data
        if len(d) < 0x200:
            raise RomError('file is shorter than a DS header')
        h = {}
        names = ['arm9_rom', 'arm9_entry', 'arm9_ram', 'arm9_size',
                 'arm7_rom', 'arm7_entry', 'arm7_ram', 'arm7_size',
                 'fnt_off', 'fnt_size', 'fat_off', 'fat_size',
                 'ovt9_off', 'ovt9_size', 'ovt7_off', 'ovt7_size']
        for i, n in enumerate(names):
            h[n] = u32(d, 0x20 + 4 * i)
        h['icon_off'] = u32(d, 0x68)
        h['arm9_autoload_hook'] = u32(d, 0x70)
        h['arm7_autoload_hook'] = u32(d, 0x74)
        h['rom_size'] = u32(d, 0x80)
        h['header_size'] = u32(d, 0x84)
        h['header_crc'] = u16(d, 0x15E)
        self.header = h
        self.title = d[0:12].split(b'\0')[0].decode('ascii', 'replace')
        self.gamecode = d[12:16].decode('ascii', 'replace')
        self.maker = d[16:18].decode('ascii', 'replace')
        self.unitcode = d[0x12]
        if crc16(d[:0x15E]) != h['header_crc']:
            raise RomError(f'header CRC mismatch (stored {h["header_crc"]:#06x}, computed {crc16(d[:0x15E]):#06x}): not a DS ROM, or a damaged one')
        for cpu in (9, 7):
            off, size, ram = h[f'arm{cpu}_rom'], h[f'arm{cpu}_size'], h[f'arm{cpu}_ram']
            if off + size > len(d):
                raise RomError(f'ARM{cpu} binary runs past the end of the file')
        self.arm7 = d[h['arm7_rom']:h['arm7_rom'] + h['arm7_size']]
        self.arm9 = d[h['arm9_rom']:h['arm9_rom'] + h['arm9_size']]
        self._parse_arm9()
        self._parse_fs()
        self.overlays9 = self._parse_ovt(h['ovt9_off'], h['ovt9_size'])
        self.overlays7 = self._parse_ovt(h['ovt7_off'], h['ovt7_size'])

    def _find_params(self, img):
        # The signature is two words, big-endian then little-endian "nitrocode".
        sig = struct.pack('<II', NITROCODE_BE, NITROCODE_LE)
        pos = img.find(sig)
        while pos >= 0 and pos % 4:
            pos = img.find(sig, pos + 1)
        if pos < 0x1C:
            return None
        o = pos - 0x1C
        w = struct.unpack_from('<7I', img, o)
        return ModuleParams(o, *w)

    def _parse_arm9(self):
        h = self.header
        ram = h['arm9_ram']
        img = self.arm9
        p = self._find_params(img)
        self.params = p
        if p is None:
            return      # homebrew: a plain binary
        if p.compressed_end:
            end = p.compressed_end - ram
            if not 0 < end <= len(img):
                raise RomError(f'compressed static end {p.compressed_end:#x} lies outside the ARM9 image')
            img = blz_decompress(img[:end]) + img[end:]
            self.arm9_compressed = True
            # The decompressed image no longer needs decompressing: clear the field, as
            # the SDK's own routine does once it has run.
            b = bytearray(img)
            struct.pack_into('<I', b, p.offset + 0x14, 0)
            img = bytes(b)
            p.compressed_end = 0
        self.arm9 = img
        # Autoload segments: a list of (destination, size, bss size) whose data is
        # packed one after another from autoload_start.
        lst = p.autoload_list - ram
        lst_end = p.autoload_list_end - ram
        src = p.autoload_start - ram
        if 0 <= lst <= lst_end <= len(img) and 0 <= src <= len(img):
            for i in range((lst_end - lst) // 12):
                dest, size, bss = struct.unpack_from('<3I', img, lst + 12 * i)
                if src + size > len(img) or size > 0x400000:
                    raise RomError(f'autoload segment {i} ({dest:#x}, {size:#x} bytes) runs past the ARM9 image')
                self.autoloads.append(Segment(f'autoload{i}@{dest:08x}', 9, dest, img[src:src + size], bss, 'autoload'))
                src += size

    def _parse_fs(self):
        h = self.header
        d = self.data
        self.fat = [struct.unpack_from('<II', d, h['fat_off'] + 8 * i) for i in range(h['fat_size'] // 8)]
        fnt = h['fnt_off']
        if not h['fnt_size']:
            return
        ndirs = u16(d, fnt + 6)
        if ndirs == 0 or ndirs > 4096:
            return

        def walk(dir_id, prefix):
            e = fnt + 8 * (dir_id & 0xFFF)
            sub_off, first_file = u32(d, e), u16(d, e + 4)
            p = fnt + sub_off
            fid = first_file
            while True:
                t = d[p]
                p += 1
                if t == 0:
                    break
                n = t & 0x7F
                name = d[p:p + n].decode('latin-1')
                p += n
                if t & 0x80:
                    sub = u16(d, p)
                    p += 2
                    walk(sub, prefix + name + '/')
                else:
                    self.files[prefix + name] = fid
                    fid += 1
        try:
            walk(0xF000, '')
        except (IndexError, struct.error):
            raise RomError('file name table is damaged')

    def _parse_ovt(self, off, size):
        out = []
        for i in range(size // 32):
            w = struct.unpack_from('<8I', self.data, off + 32 * i)
            o = Overlay(*w)
            if o.file_id < len(self.fat):
                o.rom_start, o.rom_end = self.fat[o.file_id]
            out.append(o)
        return out

    # -- access ---------------------------------------------------------------
    def file(self, fid):
        s, e = self.fat[fid]
        return self.data[s:e]

    def overlay_image(self, ov):
        img = self.file(ov.file_id)
        if ov.compressed:
            img = blz_decompress(img[:ov.compressed_size] if ov.compressed_size else img)
        return img

    def segments(self, cpu=9, overlays=True):
        """Every run of code the CPU can have, at its RAM address."""
        h = self.header
        if cpu == 7:
            segs = [Segment('arm7', 7, h['arm7_ram'], self.arm7)]
            if overlays:
                for ov in self.overlays7:
                    segs.append(Segment(f'ov7_{ov.id:03d}', 7, ov.ram_addr, self.overlay_image(ov), ov.bss_size, 'overlay', ov.id))
            return segs
        static = self.arm9
        if self.params is not None and self.params.autoload_start:
            # The autoload data is copied away at start-up; the static image proper ends there.
            static = static[:self.params.autoload_start - h['arm9_ram']]
        segs = [Segment('arm9', 9, h['arm9_ram'], static)]
        segs += self.autoloads
        if overlays:
            for ov in self.overlays9:
                segs.append(Segment(f'ov9_{ov.id:03d}', 9, ov.ram_addr, self.overlay_image(ov), ov.bss_size, 'overlay', ov.id))
        return segs


def crc16(data, crc=0xFFFF):
    """The CRC-16 (MODBUS polynomial) the DS uses for its header and the BIOS GetCRC16."""
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_rom(arm9, arm9_ram, arm9_entry, arm7, arm7_ram, arm7_entry, title='DSNATIVE', gamecode='DSNT', files=None):
    """Assemble a minimal .nds image: header, ARM9, ARM7 and an optional flat file system.
    Used by the tests and by tools/mknds.py for homebrew."""
    def align(b, a=0x200):
        return b + b'\0' * ((-len(b)) % a)
    files = files or {}
    hdr = bytearray(0x4000)
    out = bytearray()
    arm9_off = 0x4000
    body = align(bytes(arm9))
    arm7_off = arm9_off + len(body)
    body += align(bytes(arm7))
    # File system: one root directory with plain files.
    names = sorted(files)
    fnt = bytearray(struct.pack('<IHH', 8, 0, 1))
    for n in names:
        nb = n.encode('latin-1')
        fnt += bytes([len(nb)]) + nb
    fnt += b'\0'
    fnt_off = arm9_off + len(body)
    body += align(bytes(fnt))
    fat_off = arm9_off + len(body)
    fat = bytearray()
    data_off = fat_off + len(align(b'\0' * (8 * len(names)) or b'\0'))
    blobs = bytearray()
    for n in names:
        s = data_off + len(blobs)
        blobs += files[n]
        fat += struct.pack('<II', s, s + len(files[n]))
        blobs = bytearray(align(bytes(blobs), 4))
    body += align(bytes(fat) or b'\0')
    body += bytes(blobs)
    hdr[0:12] = title.encode()[:12].ljust(12, b'\0')
    hdr[12:16] = gamecode.encode()[:4]
    hdr[16:18] = b'00'
    struct.pack_into('<16I', hdr, 0x20,
                     arm9_off, arm9_entry, arm9_ram, len(arm9),
                     arm7_off, arm7_entry, arm7_ram, len(arm7),
                     fnt_off, len(fnt), fat_off, len(fat),
                     0, 0, 0, 0)
    struct.pack_into('<I', hdr, 0x60, 0x00586000)
    struct.pack_into('<I', hdr, 0x80, arm9_off + len(body))
    struct.pack_into('<I', hdr, 0x84, 0x4000)
    struct.pack_into('<H', hdr, 0x15E, crc16(hdr[:0x15E]))
    out = bytes(hdr) + bytes(body)
    return out
