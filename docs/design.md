# dsnative: design

## The problem

A DS game is two programs: one for the ARM946E-S (ARMv5TE, 67 MHz) and one for the ARM7TDMI
(ARMv4T, 33 MHz). They share 4 MB of RAM and talk through IPC registers. Neither runs under an
operating system: games link Nintendo's Nitro SDK and program the hardware registers directly.
Most of the ARM7 program is the SDK's own component (sound, touch, RTC, Wi-Fi, power), which is
nearly the same in every game.

Apple silicon can't execute 32-bit ARM, and iOS and tvOS don't allow generating code at run
time. So the code is translated ahead of time into C, which Clang compiles for the target.

## Recompilation

### Discovery (`recomp/program.py`)

Code is found by walking it from the entry points:

- **Calls** (BL, BLX imm, Thumb BL pairs) make their targets function entries, and the
  instruction after the call a return site.
- **Branches** (B, conditional B) stay inside the function, unless their target is another
  function's entry, a different instruction set state or a different segment.
- **Indirect calls** (BLX reg, and the ARMv4 `mov lr, pc` before `bx`/`ldr pc`) leave to a
  computed address. The next instruction is a return site. When the register was loaded from
  a literal (`ldr r3, =main; blx r3`, as crt0 calls `main`), its value is a function entry.
- **Jump tables**: `add pc, pc, rN, lsl #2` (a table of branches) and `ldr pc, [pc, rN, lsl #2]`
  (a table of addresses), bounded by the `cmp rN, #n` just before; and GCC 15's form, where
  `ldrb rN, [rT, rI]` (or `ldrh` of a doubled index) reads the case's offset in words from a
  table in rodata just before the `add pc`, bounded by `cmp rI, #n`. A `cmp` only bounds a
  table when nothing between it and the jump writes the register.
- **Interworking veneers**: Thumb `bx pc` jumps to the ARM code 4 bytes on.
- **Pointer candidates**: every literal-pool word and every data word that points into a code
  segment is a candidate entry. It is taken only when the code there looks like the start of
  a function: it starts unconditionally (ARM) and decodes, with no undefined instruction,
  for six instructions or up to a branch or return. A false candidate costs code size, never
  correctness.
- **Certain and speculative code**: code walked from the entry points is certain; code reached
  only from candidates is speculative. Only certain code hides a word from the data scan: a
  candidate that is really data (a string at an odd address reads as plausible Thumb code)
  can walk on through a table of function pointers, and the table must still be scanned.
  Certain walks run first, so they see each function from its start, with the `cmp` before
  a jump table, before a candidate lands in the middle of it.
- **Feedback**: when the runtime reaches an address nothing was compiled for, it stops and
  appends the address to `missing.txt`. `--extra missing.txt` adds it to the next build. The
  game profile (planned) keeps such entries per game.

Segments are the ARM9 static image, each autoload segment at its destination (ITCM, DTCM, other
RAM), each overlay at its load address, and the ARM7 image. A call from an overlay into an
address covered by several overlays becomes an entry in each of them.

### Grouping and C output

Each function entry becomes a C function holding every block reachable from it by branches.
A label inside it marks each block start:

```c
u32 g9_s0_02000a10_t(Cpu *c, u32 pc) {
    switch (pc) { case 0x2000a10u: goto L_02000a10; case 0x2000a2cu: goto L_02000a2c; ... }
L_02000a10:
    c->budget -= 7;
    /* 02000a10: push {r4, r5, r6, lr} */
    { u32 base = R[13]; u32 a = base - 16; ... }
    ...
    /* 02000a24: bl #0x2001ed0 */
    { R[14] = 0x2000a29u; return 0x2001ed0u; }
L_02000a2c:
    ...
}
```

The function's return value is the next guest address. Every label is registered in the
dispatch table, so execution can resume at any block start, including a return site, a
jump-table target, or the instruction after a BIOS call or a CPSR write.

### Instruction semantics (`recomp/isa.py`)

Each instruction becomes a C block using the helpers in `runtime/dsr.h`. They cover:

- **Flags**: N, Z, C, V and Q are separate words, so the compiler drops the ones nothing reads.
- **Shifter operands**: immediate shifts are folded at build time. Register shifts follow the
  architecture's rules for amounts of 0, 32 and above.
- **Loads and stores**: unaligned LDR rotates. LDRH and LDRSH differ between the ARM9 and the
  ARM7. Writeback order and the "base in register list" rules also differ between ARMv4 and
  ARMv5.
- **Interworking**: ARMv5 loads to the PC switch state, ARMv4 loads don't. Data-processing
  writes to the PC never do. `S` with Rd = PC is an exception return.
- **ARMv5TE**: CLZ, QADD/QSUB/QDADD/QDSUB, the SMLAxy family, LDRD/STRD, BLX, PLD.
- **System**: MRS/MSR, user-bank LDM/STM, SWI (the BIOS, in C), MCR/MRC to CP15.

`tests/difftest.py` checks all of this. It generates random snippets of ARM and Thumb code
(data processing, multiplies, DSP instructions, every load and store form, block transfers,
conditional branches), runs each from the same random state in Unicorn's ARM946 and natively
after recompilation, and compares registers, flags and memory.

## Runtime

### Dispatch (`runtime/cpu.c`)

`dsr_run(cpu, cycles)` loops:

1. take a pending interrupt;
2. run BIOS code if the PC is in the BIOS (the interrupt entry and return, and the IntrWait
   loop, all written in C);
3. look up the address (a direct-mapped cache in front of a binary search of the table);
4. call the function.

Overlay code carries the first bytes it was compiled from. The dispatcher compares them with
memory, so the right overlay runs when several share an address.

Because no guest frame lives on the host stack, guest threads (the Nitro OS switches contexts
by rewriting SP and PC), interrupts and nested exceptions work as they do on the hardware.

### The machine (`runtime/nds.c`, `runtime/gpu2d.c`)

**Memory** goes through 4 KB page tables, one each for reads, 16/32-bit writes and 8-bit
writes (VRAM ignores 8-bit writes from the ARM9). The tables cover main RAM, shared and ARM7
WRAM, VRAM banks as VRAMCNT maps them, and the TCMs as CP15 places them. Everything else
(I/O, palettes, OAM) takes a slow path.

**Timing** is by scanline. Each line is 1536 + 594 bus cycles (visible part, then HBlank),
run in slices of 256 bus cycles: 512 ARM9 cycles, then 256 ARM7 cycles. Instructions are
counted as one cycle each for now.

**Hardware**:

- **Interrupts**: IME, IE and IF per CPU; the BIOS interrupt dispatch through the handler
  pointer at DTCM+0x3FFC (ARM9) or 0x0380FFFC (ARM7); IntrWait with its check flags.
- **Display timing**: DISPSTAT and VCOUNT, with VBlank, HBlank and VCount interrupts.
- **DMA**: immediate, VBlank, HBlank, cartridge and geometry FIFO timing, plus the ARM9 DMA
  fill registers.
- **Timers**, including cascades.
- **IPC**: IPCSYNC and the two 16-word FIFOs, with their interrupts.
- **Divider and square root** (ARM9), with the hardware's divide-by-zero results.
- **Cartridge**: B7 reads, chip ID, interrupts and DMA, including the secure-area
  redirection below 0x8000.
- **SPI** (ARM7): the power manager, the firmware flash (with generated user settings) and the
  touch screen controller.
- **Direct boot**: the binaries in RAM, the header and chip IDs where the firmware puts them,
  the firmware user settings, registers and stacks.
- **2D engines**, one scanline at a time: text, affine, extended affine and bitmap
  backgrounds, regular and affine sprites, priorities, alpha blending, brightness effects,
  master brightness and VRAM display mode.

## Roadmap

### 1. Proof of concept (done)

Recompiler, runtime and the homebrew end-to-end test.

### 2. A commercial game to its title screen

What a Nitro SDK game needs that homebrew doesn't, roughly in the order a game hits it:

- **Save memory** on the cartridge SPI bus: EEPROM and FLASH (Pokémon Black 2 uses 512 KB of
  FLASH), with a save file on disk.
- **RTC**: the bit-banged serial clock on the ARM7. Pokémon reads the date and time.
- **Sound**: the 16 channels (PCM8, PCM16, IMA-ADPCM, PSG, noise), capture, and mixing to
  32 kHz. The ARM7's sound driver only programs these registers.
- **3D**: the geometry engine (matrix stacks, lighting, the command FIFO and its DMA, box and
  position tests) and the rasterizer (textures, toon/highlight, fog, edge marking,
  anti-aliasing, alpha), feeding the 2D engine's BG0. A software rasterizer comes first, then
  Metal. Pokémon Black 2's overworld is 3D.
- **The rest of the 2D engines**: windows, mosaic, bitmap sprites, the large bitmap mode,
  main-memory display, display capture (used for motion blur and dual-screen 3D).
- **Wi-Fi**: enough register stubs for the SDK to initialize and report no network.
- **Game profiles**: per game (by game code and revision), the extra entry points, known
  data-in-code regions and patches. Like nxnative's `dm/games/<id>/`.
- **DSi-enhanced titles** run in DS mode (Black 2 is one); DSi mode is out of scope for now.

### 3. An Apple app

A Swift/SwiftUI shell around the C runtime, built by Xcode from the generated sources:

- **Screens**: Metal output of both screens, laid out for each device. Two screens where the
  device has them, stacked or side by side on a single display, and a TV layout for Apple TV.
- **Input**: touch on the bottom screen (mapped through the SPI touch controller), game
  controllers through GameController.framework, an on-screen pad on iPhone, and the Siri
  Remote as a minimal pad on tvOS.
- **Audio**: AVAudioEngine fed by the sound mixer.
- **Saves and settings**: in the app's documents, exportable.
- **Packaging**: a script that turns a ROM into an IPA or Mac app, as nxnative's
  `build_ipa_from_nsp.py` does, so the existing Switch launchers (NX2Apple, NX Play) could
  add DS games alongside Switch ones.

### 4. Speed

The DS is slow by modern standards, so correctness comes first. Then:

- keep guest registers in host locals within a function (today every access goes through the
  `Cpu` structure, and memory writes force reloads);
- direct C calls for static BL targets, falling back to the dispatcher on a mismatched return;
- flag liveness, to drop dead flag computations;
- cycle counts per instruction class, for games that depend on timing.

### 5. 3DS

The same recompiler grows to ARMv6K (the ARM11 MPCore: more instructions, VFPv2, the
multiprocessor primitives). The runtime grows a Horizon layer: the kernel's SVCs and the
services `gsp`, `hid`, `fs`, `apt`, `dsp` and `ndm`, implemented at a high level as nxnative
does for the Switch. PICA200 shaders get translated to Metal ahead of time, and the
fixed-function TEV stages become generated fragment shaders.
