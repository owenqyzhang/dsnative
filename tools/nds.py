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


def _blz_in_place(packed):
    """Decompress `packed` the way a crt0 does, in one buffer, writing backwards from the end
    of the output while reading backwards from the footer. Returns the result, or None when a
    write would land on stream bytes not read yet (literals cost a flag bit each, so a stretch
    that doesn't compress can make the output pointer overtake the input)."""
    n = len(packed)
    inc_len = u32(packed, n - 4)
    hdr = u32(packed, n - 8)
    stop = n - (hdr & 0xFFFFFF)
    buf = bytearray(packed) + bytearray(inc_len)
    src, dst = n - (hdr >> 24), n + inc_len
    while src > stop:
        src -= 1
        flags = buf[src]
        for _ in range(8):
            if dst <= stop or src <= stop:
                break
            if flags & 0x80:
                src -= 2
                v = buf[src] | buf[src + 1] << 8
                length, disp = (v >> 12) + 3, (v & 0xFFF) + 3
                for _ in range(length):
                    if dst <= stop:
                        break
                    dst -= 1
                    if dst < src:
                        return None
                    buf[dst] = buf[dst + disp]
            else:
                src -= 1
                dst -= 1
                if dst < src:
                    return None
                buf[dst] = buf[src]
            flags = (flags << 1) & 0xFF
    return bytes(buf)


def blz_compress(data, keep=0):
    """Compress data with BLZ, leaving its first `keep` bytes (at least) uncompressed.

    Only used to build test images (a real game is compressed by the Nitro SDK). Like the SDK's
    own tools it makes sure the result decompresses in place, which is how a crt0 does it: when
    the first stretch of the compressed part doesn't shrink, more of the start stays
    uncompressed. The output decompresses with blz_decompress and with _blz_in_place."""
    data = bytes(data)
    for k in range(keep, len(data), 4):
        packed = _blz_encode(data, k)
        if packed is None:
            return None
        if _blz_in_place(packed) == data:
            return packed
    return None


def _blz_encode(data, keep):
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
    segment (ITCM, DTCM or another), a start-up stub, an overlay or the ARM7 image."""
    name: str
    cpu: int            # 9 or 7
    addr: int
    data: bytes
    bss: int = 0
    kind: str = 'static'    # static, autoload, stub, overlay; pic: code that runs wherever it is copied (addr is 0)
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
    arm9_stored: bytes = b''       # the ARM9 image as the cartridge holds it (BLZ-compressed or not)
    params: ModuleParams = None
    autoloads: list = field(default_factory=list)   # list of Segment (ARM9)
    params7: ModuleParams = None
    autoloads7: list = field(default_factory=list)  # list of Segment (ARM7)
    stubs: dict = field(default_factory=dict)       # cpu -> list of Segment: code crt0 copies and runs
    pic_stubs: dict = field(default_factory=dict)   # cpu -> list of Segment (kind 'pic', no address): code run from the stack
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
        self.params7 = self._find_params(self.arm7) or self._find_params_unsigned(self.arm7, h['arm7_ram'])
        if self.params7 is not None:
            self.autoloads7 = self._parse_autoloads(7, self.arm7, h['arm7_ram'], self.params7)
        self.stubs = {9: self._find_stubs(9, self.arm9, h['arm9_ram']), 7: self._find_stubs(7, self.arm7, h['arm7_ram'])}
        self.pic_stubs = {9: self._find_pic_stubs(9, self.arm9, h['arm9_ram']), 7: self._find_pic_stubs(7, self.arm7, h['arm7_ram'])}
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

    @staticmethod
    def _find_params_unsigned(img, ram):
        """SDK 4's ARM7 crt0 has the module parameters without the signature or the SDK version:
        six words (autoload list, its end, the autoload data, static bss start and end, the
        compressed end) that its start-up code reaches through a literal. The words of the
        first 0x400 bytes that point into the image are tried as that address; one is a table
        when its list is at the end of an autoload data whose entries' sizes add up to the
        distance between them, which is hard to hit by chance."""
        for o in range(0, min(len(img), 0x400) - 3, 4):
            q = u32(img, o) - ram
            if not 0 <= q <= len(img) - 24 or q % 4:
                continue
            lst, end, src, bss0, bss1, comp = struct.unpack_from('<6I', img, q)
            lst, end, src = lst - ram, end - ram, src - ram
            if not 0 < src <= lst < end == len(img) or (end - lst) % 12 or comp:
                continue
            if sum(u32(img, e + 4) for e in range(lst, end, 12)) == lst - src:
                return ModuleParams(q, ram + lst, ram + end, ram + src, bss0, bss1, comp, 0)
        return None

    def _parse_arm9(self):
        h = self.header
        ram = h['arm9_ram']
        img = self.arm9
        self.arm9_stored = img
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
            # The decompressed image no longer needs decompressing: clear the field so that
            # discovery sees the program as it is after crt0 ran. The SDK's crt0 does not clear
            # it: the field stays set in RAM (the module parameters are in the uncompressed
            # head, which a game's reload from the cartridge leaves alone), so a restart that
            # reloads the compressed body decompresses it again. `arm9_stored` keeps the image
            # as the cartridge has it, for the runtime to boot from.
            b = bytearray(img)
            struct.pack_into('<I', b, p.offset + 0x14, 0)
            img = bytes(b)
            p.compressed_end = 0
        self.arm9 = img
        self.autoloads = self._parse_autoloads(9, img, ram, p)

    @staticmethod
    def _parse_autoloads(cpu, img, ram, p):
        """Autoload segments: a list of (destination, size, bss size) whose data is packed
        one after another from autoload_start. SDK 5 entries have a fourth word,
        (destination, size, ?, bss size); the third repeats the destination in the games
        seen so far. The entry size is the one whose sizes add up to the packed data."""
        out = []
        lst = p.autoload_list - ram
        lst_end = p.autoload_list_end - ram
        src = p.autoload_start - ram
        if 0 <= lst <= lst_end <= len(img) and 0 <= src <= len(img):
            step = 12
            for n in (12, 16):
                if (lst_end - lst) % n == 0 and \
                        sum(u32(img, o + 4) for o in range(lst, lst_end, n)) == lst - src:
                    step = n
                    break
            for i in range((lst_end - lst) // step):
                e = struct.unpack_from(f'<{step // 4}I', img, lst + step * i)
                dest, size, bss = e[0], e[1], e[-1]
                if src + size > len(img) or size > 0x400000:
                    raise RomError(f'ARM{cpu} autoload segment {i} ({dest:#x}, {size:#x} bytes) runs past its image')
                name = f'autoload{i}@{dest:08x}' if cpu == 9 else f'arm7_autoload{i}@{dest:08x}'
                out.append(Segment(name, cpu, dest, img[src:src + size], bss, 'autoload'))
                src += size
        return out

    @staticmethod
    def _find_stubs(cpu, img, ram):
        """Code crt0 copies to fast memory and calls before the autoloads (SDK 5: the two
        CPUs' start-up handshake, run from ITCM and ARM7 WRAM):
            ldr r1, =src; ldr r2, =dst; add r3, r2, #n
            1: ldr r0, [r1], #4; str r0, [r2], #4; cmp r2, r3; blt 1b"""
        loop = struct.pack('<4I', 0xE4910004, 0xE4820004, 0xE1520003, 0xBAFFFFFB)
        out = []
        i = img.find(loop)
        while i >= 0:
            if i % 4 == 0 and i >= 12:
                a, b, c = struct.unpack_from('<3I', img, i - 12)
                if (a & 0xFFFFF000) == 0xE59F1000 and (b & 0xFFFFF000) == 0xE59F2000 and (c & 0xFFFFFF00) == 0xE2823000:
                    la, lb = i - 12 + 8 + (a & 0xFFF), i - 8 + 8 + (b & 0xFFF)
                    if lb + 4 <= len(img):
                        src, dst, n = u32(img, la) - ram, u32(img, lb), c & 0xFF
                        if n % 4 == 0 and 0 <= src and src + n <= len(img):
                            out.append(Segment(f'stub{len(out)}@{dst:08x}', cpu, dst, img[src:src + n], 0, 'stub'))
            i = img.find(loop, i + 1)
        return out

    @staticmethod
    def _find_pic_stubs(cpu, img, ram):
        """Code start-up copies onto its stack and jumps to, so it runs wherever the stack is
        and has no address (SDK 5's ARM7 crt0 ends this way: the last handshake write,
        strh r2, [r1]; bx r0). Each `bx rN` is run backwards-first: the straight-line code
        before it, with what it loads from the image known and the stack as offsets from
        the sp it started with. If rN is then an address on the stack, the stub is the words
        stored from there up to the first bx. A store the run can't place, or an instruction
        it doesn't model, forgets the stack: only a copy it follows to the end is a stub."""
        n = len(img) // 4
        out, seen = [], set()
        for i in range(n):
            w = u32(img, 4 * i)
            if w & 0xFFFFFFF0 != 0xE12FFF10 or (w & 0xF) in (14, 15):
                continue
            code = _stack_copy(img, ram, i, w & 0xF)
            if code is not None and code not in seen:
                seen.add(code)
                out.append(Segment(f'pic{len(out)}', cpu, 0, code, 0, 'pic'))
        return out

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
            static = self.arm7
            if self.params7 is not None and self.params7.autoload_start:
                static = static[:self.params7.autoload_start - h['arm7_ram']]
            segs = [Segment('arm7', 7, h['arm7_ram'], static)] + self.autoloads7 + self.stubs.get(7, [])
            if overlays:
                for ov in self.overlays7:
                    segs.append(Segment(f'ov7_{ov.id:03d}', 7, ov.ram_addr, self.overlay_image(ov), ov.bss_size, 'overlay', ov.id))
            return segs
        static = self.arm9
        if self.params is not None and self.params.autoload_start:
            # The autoload data is copied away at start-up; the static image proper ends there.
            static = static[:self.params.autoload_start - h['arm9_ram']]
        segs = [Segment('arm9', 9, h['arm9_ram'], static)]
        segs += self.autoloads + self.stubs.get(9, [])
        if overlays:
            for ov in self.overlays9:
                segs.append(Segment(f'ov9_{ov.id:03d}', 9, ov.ram_addr, self.overlay_image(ov), ov.bss_size, 'overlay', ov.id))
        return segs


def _stack_copy(img, ram, at, rn, window=32):
    """The code the straight-line instructions before the `bx rn` at word `at` of img (loaded
    at ram) copied to the stack and rn points at, or None. See Rom._find_pic_stubs.

    A value is an int, or ('sp', k) for the sp the run started with, + k; unknown values are
    left out of `regs`, and `mem` holds the words stored on the stack, by offset."""
    regs, mem = {}, {}

    def reset():
        regs.clear()
        mem.clear()
        regs[13] = ('sp', 0)        # a new base: nothing stored before is on this stack

    def word(a):
        o = a - ram
        return u32(img, o) if a % 4 == 0 and 0 <= o <= len(img) - 4 else None

    def plus(v, d):
        if isinstance(v, int):
            return (v + d) & 0xFFFFFFFF
        return ('sp', v[1] + d) if v is not None else None

    def put(r, v):
        if v is None:
            regs.pop(r, None)
        else:
            regs[r] = v

    reset()
    for j in range(max(0, at - window), at):
        w = u32(img, 4 * j)
        op, opc = (w >> 25) & 7, (w >> 21) & 0xF
        rd, rb = (w >> 12) & 0xF, (w >> 16) & 0xF
        if w >> 28 != 0xE or rd == 15:
            reset()
        elif op == 1 and opc in (2, 4, 13) and not w & (1 << 20):      # sub, add, mov rd, #imm
            rot = ((w >> 8) & 0xF) * 2
            imm = (((w & 0xFF) >> rot) | ((w & 0xFF) << (32 - rot))) & 0xFFFFFFFF
            put(rd, imm if opc == 13 else plus(regs.get(rb), imm if opc == 4 else -imm))
        elif (w & 0x0FFF0FF0) == 0x01A00000:                            # mov rd, rm
            put(rd, regs.get(w & 0xF))
        elif op == 2 and not w & (1 << 22) and not (w & (1 << 21) and (rb == rd or not w & (1 << 24))):
            # ldr/str rd, [rn, #+-imm]{!} or [rn], #+-imm (not the byte forms, nor ldrt/strt)
            off = (w & 0xFFF) if w & (1 << 23) else -(w & 0xFFF)
            pre = bool(w & (1 << 24))
            base = ram + 4 * j + 8 if rb == 15 else regs.get(rb)
            ea = plus(base, off) if pre else base
            if w & (1 << 20):
                if isinstance(ea, int):
                    put(rd, word(ea))
                else:
                    put(rd, mem.get(ea[1]) if ea is not None else None)
            elif isinstance(ea, tuple):
                v = regs.get(rd)
                mem[ea[1]] = v if isinstance(v, int) else None
            else:
                mem.clear()         # a store through a base this run doesn't know may hit the stack
            if rb != 15 and (not pre or w & (1 << 21)):
                put(rb, plus(base, off) if not pre else ea)
        else:
            reset()
    v = regs.get(rn)
    if not isinstance(v, tuple) or v[1] % 4:
        return None
    words, k = [], v[1]
    while mem.get(k) is not None:
        words.append(mem[k])
        k += 4
        if words[-1] & 0xFFFFFFF0 == 0xE12FFF10:
            return b''.join(struct.pack('<I', x) for x in words)
    return None


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
    # Card reads below 0x8000 are redirected (secure area protection): the file system
    # goes above it, as on real cartridges.
    if arm9_off + len(body) < 0x8000:
        body += b'\0' * (0x8000 - arm9_off - len(body))
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
