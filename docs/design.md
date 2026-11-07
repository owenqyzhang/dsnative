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
- **Jump targets** (`recomp/jumps.py`): a computed jump is read by an evaluator, not a matcher
  per encoding. It takes the straight path of instructions before the jump and runs it
  symbolically. The guards on the path (a `cmp` and the branch after it, or a `bls`/`bhi` over
  a `b default`, in ARM and Thumb) bound the index; the evaluator then runs the path once per
  value of the index and reads each case's target from the memory image. A table of branches,
  of words, of byte or halfword offsets, a CodeWarrior Thumb switch and GCC 15's address table
  (ARMv4T Thumb: `ldr`, then `mov pc`) all come out as one list of (address, Thumb) cases, one
  per index value in increasing order. A constant (`ldr r3, =main; blx r3`) is a function
  entry. All of it is read from memory, so it doesn't matter which walk reaches the jump.
- **What bounds a table**: a call on the path is a join, so a bound from before it doesn't bound
  the table after it, unless nothing else does. The path is read twice: first with no
  assumptions, then with the assertion reading, which treats a call that the branch before it
  skips as one that never returns (`cmp r0, #2; bls 1f; bl panic; 1:`); the second reading can
  only bound a table the first left unbounded. Tests for equality (a zero check before the
  switch) cut no case out of a table: they hold only if the branch is the one way into the
  code, which the path can't tell, and a case kept in costs only code size. A loop's head is a
  join whose back edge comes after the jump, so a run that is infeasible, or finds no table
  where only a table explains the jump, restarts the path later.
- **Unbounded `add pc, pc, rN, lsl #2`** lands in a run of branches (a table) or in straight-line
  code (an unrolled loop entered part way, as the SDK's division is), and every instruction to
  the end of the run becomes a label. Any other jump without a bound is reported as a warning.
- **Saved contexts**: an ARM `adr rN, x` makes `x - 4` a label when it is code. The Nitro OS
  saves a thread's resume point that way (`OS_SaveContext` stores its own `bx lr` + 4) and
  resumes it with `subs pc, lr, #4`.
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
  Certain walks run first, so they see each function from its start before a candidate lands
  in the middle of it.
- **Addresses several segments share** (overlays loaded at one address, or a stub and the
  autoload copied over it): a call, a branch or a constant call there enters each of those
  segments, and each walk is speculative, because at most one of them is the code that runs and
  the others hold other overlays' code or data. Data decodes as code (a table of function
  pointers reads as `andeq r0, r0, #imm`, which as certain code would cover its own words and
  hide its pointers from the scan). Each segment still becomes an entry and is compiled, and
  code at such an address is checked against memory when dispatched. A call or branch inside
  the caller's own segment stays in it (an overlay is loaded while its own code runs), and one
  into an address only one segment covers stays certain. In Black 2 almost all ARM9 code is
  reached this way: 1,639,878 of 1,674,047 walked instructions come from candidates and 34,169
  are certain, because 344 overlays share addresses.
- **Feedback**: when the runtime reaches an address nothing was compiled for, it stops and
  appends the address to `missing.txt`. `--extra missing.txt` adds it to the next build. The
  game profile (planned) keeps such entries per game.

Segments are the ARM9 and ARM7 static images, each autoload segment at its destination (ITCM,
DTCM, ARM7 WRAM, other RAM; SDK 5 lists them in four-word entries), each start-up stub at the
address crt0 copies it to, and each overlay at its load address.

Start-up stubs are code crt0 copies to fast memory and calls before the autoloads overwrite
it: in SDK 5 the two CPUs' handshake, run from ITCM and from ARM7 WRAM (the reader finds the
`ldr r1, =src; ldr r2, =dst; add r3, r2, #n` copy loop).

Position-independent stubs: SDK 5's ARM7 crt0 copies its last two words (`strh r2, [r1]; bx r0`)
to its own stack and jumps to them, so the code has no address. The ROM reader runs the
straight-line code before each `bx rN` with what it loads from the image known and the stack as
offsets from sp, finds the copy, and returns the words as a stub of kind `pic`, which is not a
segment. The recompiler turns each into a function of its own, accepted only if its
instructions read no address (it decodes them at two addresses and compares: a pc read, a
literal load or a branch makes the code differ) and it ends in one unconditional register
jump, and writes the functions and their bytes to `g9_pic.c` / `g7_pic.c`, longest first. On a
dispatch miss the dispatcher runs the stub whose bytes are in RAM at `pc`. `ndsinfo --code`
lists these stubs and the jump table counts.

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
- **RTC** (`runtime/rtc.c`, ARM7 0x04000138): the Seiko S-35180, a 3-wire serial clock the ARM7
  bit-bangs. CS rising starts a transfer. The chip reads SIO on each rising SCK edge (a command
  byte with fixed code 0110, a 3-bit command and the read/write bit, then the parameters, all
  LSB first). When reading, it drives SIO from each falling edge: the Nitro SDK samples while
  SCK is still low, GBATEK's flowchart once SCK is high again, and both see the same bit. SIO is
  modelled as open drain. The registers are status 1 and 2 (reset, 12/24-hour mode, flags
  cleared on read, the INT1 mode and INT2 enable), date and time in BCD (12-hour mode counts
  0-11 with a PM bit), time only, both alarms, the INT1 frequency register (the same byte as
  alarm 1's minute), clock adjustment and the free register. Where GBATEK leaves something
  open, the S-35190A datasheet decides: invalid writes are corrected as it describes, reset
  keeps the 12/24-hour and general-purpose bits written along with it, a read captures its data
  when the command arrives, and a bad command byte is skipped. The clock starts at the host's
  local time, or at `--rtc YYYY-MM-DDTHH:MM:SS`, and advances with emulated time (`s->time`,
  33,513,982 bus cycles a second), not wall time, so a headless run is reproducible. Day of
  week counts from Sunday = 0, as the SDK does. The /INT output (alarms, per-minute edge and
  steady modes, the selected 1-16 Hz frequencies combined, 32 kHz as one interrupt a scanline)
  is the SI input of RCNT: `rtc_line`, called every scanline, raises ARM7 interrupt bit 7 on a
  falling edge when RCNT is in general-purpose mode with the SI interrupt enabled.
- **Save memory** (`runtime/save.c`, AUXSPICNT/AUXSPIDATA on whichever CPU EXMEMCNT gives the
  slot): EEPROM, FLASH and FRAM chips, kept in a save file (`--save PATH`, written at exit if it
  changed). The cartridge header doesn't say which chip a game has, so `save.c` picks it at the
  chip's first use, once all options are in: `--save-type`, then a table of game codes (Pokémon
  Black 2 and White 2 among them), then the size of the `--save` file if it equals a chip's size,
  then FLASH 512 KB. The last step is a guess, and the runtime says so on stderr, with how to
  override it. A game whose chip differs from the guess sends commands the model takes
  differently (EEPROM addresses of 1 or 2 bytes versus FLASH's 3, `02h` as write versus
  program), so its save won't work until the type is given. There is no automatic detection:
  GBATEK's way counts the bytes a game sends from the same program counter, and recompiled
  code has no program counter at an I/O access (`Cpu.r[15]` only holds where the current
  dispatch entered). Making it work would mean tracking the guest PC at I/O writes in generated
  code, which touches the recompiler and is left as future work.
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

- **Save memory**: done (see above). Pokémon Black 2 uses 512 KB of FLASH.
- **RTC**: done (see above). Pokémon's ARM7 uses the SDK's RTC routine, which bit-bangs the
  register with the sequence the unit tests use.
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
