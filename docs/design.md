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

SDK 4's ARM7 (Pokémon HeartGold and SoulSilver) has module parameters without the nitrocode
signature or the SDK version: six words (autoload list, its end, the autoload data, static bss
start and end, the compressed end) that crt0 reaches through a literal in its first 0x400 bytes.
`Rom._find_params_unsigned` tries each word of the image's start that points into the image as
that address. A candidate is taken when its list is at the end of the image, a whole number of
12-byte entries, and the entry sizes add up to the distance between the list and the autoload
data (the check `_parse_autoloads` already uses), which is hard to hit by chance; homebrew and
an ARM7 without a table give none. HeartGold's ARM7 has three autoloads (0x027E0000,
0x037F8000 and 0x06000000, the last for code in VRAM C or D that the game maps to the ARM7),
and discovery went from 4 functions to 661.

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
4. call the function; if there is none (or an overlay's check bytes no longer match), run the
   interpreter fallback below.

Overlay code carries the first bytes it was compiled from. The dispatcher compares them with
memory, so the right overlay runs when several share an address.

### Interpreter fallback (`runtime/interp.c`)

Two kinds of code have no compiled entry: code the game modifies while it runs, and code discovery
never found. Pokémon Black 2 shows both: overlay 338 patches the first word of a compiled
function with `movs r0, #0xaa; bx lr` and calls it (a write-then-execute check), and several
overlays are stored encrypted in the ROM and decrypted in RAM at run time, so the recompiler
compiled ciphertext and the dispatcher's check bytes no longer match what is in memory. Where
`dsr_run` used to stop with "no recompiled code at X", it now calls `dsr_interp(c, pc)`. That
decodes and executes ARM and Thumb instructions one at a time (ARMv5TE on the ARM9, ARMv4T on the
ARM7) until control reaches an address the dispatcher can run, and returns that address as a
recompiled function does. It generates no code, so it is allowed on iOS and tvOS, and the
compiled fast path is untouched: the interpreter is entered only on the miss path.

The semantics are those of `recomp/isa.py`. `interp.c` keeps its structure and decode order,
including the quirks (an undefined encoding stops even when its condition fails; a
data-processing write to the PC never interworks; LDRD/STRD only on the ARMv5TE and only with an
even register below LR), and uses the same `dsr.h` helpers for flags, shifter operands, loads and
stores, interworking, CP15, SWI and the user bank, so compiled and interpreted code share one
`Cpu` state and call each other freely in both directions. Budget is one per instruction, charged
whether or not the condition passes. Interrupts are taken as the dispatcher takes them, but after
every instruction rather than only between functions: after each instruction `dsr_interp_exit()`
says whether to go back to the dispatcher (a pending interrupt, the stop address, BIOS code, an
address with a matching compiled entry or PIC stub). A flow that returns because its slice ended,
it was interrupted, or it waits in Halt, IntrWait or a CP15 wait is remembered (up to four resume
addresses in `Cpu.interp_res`), so that the dispatcher bringing it back is not counted as a new
entry. Each address the interpreter enters is logged once per CPU to `missing.txt`.

Headless options: `--no-interp` keeps the old behaviour (stop at the missing address), which
discovery work should use to find gaps; `--interp-only` ignores all compiled code and interprets
everything, as a check of the interpreter against the whole machine (`test_hw` runs the homebrew
ROM this way and expects the same debug output, except the timer test, which counts cycles that
compiled code charges a block at a time); `--stats` prints the interpreted instruction counts.
Timing differs from compiled code only in where interrupts are taken.

Testing: `tests/difftest.py` runs each random snippet three ways against Unicorn: recompiled,
interpreted, and mixed (half the compiled entries dropped, so control passes between the two in
both directions); a second part runs single random instructions of every ARM and Thumb encoding
class on both CPUs (including CP15, SWI, mode switches, user-bank LDM/STM, SWP, LDRD/STRD,
undefined encodings and wild memory accesses), compiled against interpreted, comparing all
registers, CPSR and SPSR, banked registers, memory, the sequence of accesses and the stop
address. `tests/test_interp.py` patches a compiled function at run time, runs functions copied to
RAM that were never compiled, and falls from interpreted code into compiled code and back;
`tests/unit/test_interp.c` covers the budget, interrupts, BIOS waits, halts, overlay check
mismatches and the log.

Known limits: encrypted overlays are still compiled as ciphertext (dead code), and feeding
`missing.txt` back with `--extra` does not help, because the ROM still holds ciphertext; a later
step could capture the decrypted bytes from a run. Black 2 runs 3000 frames in about 11 s with the
interpreter (587 instructions interpreted to reach the overworld; about 101,000 with menu and
walking input), against about 5.5 s for compiled code alone before it, and about 20 s fully
interpreted.

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
  redirection below 0x8000. A transfer is a stream of word-ready requests: while a block is
  being read, each word the card has ready is one request for the DMA channel of the CPU that
  owns the slot (EXMEMCNT bit 11) with card timing (ARM9 timing 5, ARM7 timing 2). `card_dma`
  repeats the request until the block is read or no channel takes a word, so a channel of one
  word with repeat (the SDK's CARD library sets it up for some reads, Pokémon Black's overlay
  loads) moves the whole block, a channel of the block's size moves it in one go, and a channel
  with fewer words leaves the rest for the CPU to read through the port. It runs when the
  command starts and when a channel is enabled during the transfer; a channel of the other CPU
  gets no requests.
- **SPI** (ARM7): the power manager, the firmware flash and the touch screen controller.
- **Firmware flash** (`runtime/firmware.c`): the 256 KB flash is built when the machine is
  created, not read from a file. It holds the header (console type FFh = DS, chip size, user
  settings pointer), the Wi-Fi hardware settings after it at 2Ah (length 138h, CRC-16 from 0,
  version 0 = an original DS, channel mask, RF chip type 2, the 16 port values, baseband
  defaults, the RF table, per-channel RF words from a 22 MHz reference that reproduces GBATEK's
  channel 1 example), three access point slots and the hidden fourth at 3FA00h..3FDFFh, and two
  user settings areas at 3FE00h/3FF00h with the "settings okay" flags set. The SDK's start-up
  check reads the four slots and tests each one's CRC-16 (start value 0, over 00h..FDh) and
  status byte (FFh, not configured, is accepted). A slot of a never-configured console is zeros
  with status FFh and a valid CRC, so the check passes without any write, and no network, no
  Wi-Fi Connection user ID and no access point exist: without this, Pokémon Black 2 shows "Your
  Wi-Fi User Information may have been erased". The one console-specific value invented is the
  MAC address (a locally administered 02:"DSNAT"). The chip answers READ, FAST READ, RDID,
  RDSR, WREN, WRDI, PW, PP, PE, SE, DP and RDP, so a game that repairs a slot can write it
  (writes stay in RAM; there is no file).
- **Wi-Fi** (`runtime/wifi.c`): the ARM7's Wi-Fi hardware at 0x04800000-0x0480FFFF as a stub that
  gets the SDK's initialization through and reports no network. The ports at 0x04808000 have
  GBATEK's writable-bit masks and reset values; W_IF/W_IE feed one edge into the ARM7's IF bit 24;
  the resets, W_POWERSTATE and W_POWERFORCE work; the baseband chip (ID 6Dh) and the RF chip sit
  behind serial interfaces that are always ready; W_RANDOM follows its sequence, the microsecond
  counter runs on emulated time, and the 8 KB Wi-Fi RAM has the TX and RX buffer ports. Nothing
  is ever received, and transmit requests are accepted and not carried out. Black 2's ARM7
  touches 75 distinct ports and runs a RAM test over the whole 8 KB, after which its
  initialization completes.
- **DSi registers in DS mode** (`runtime/dsi.c`): what a DSi answers while running a DS cartridge.
  With SCFG_EXT's bit 31 clear, SCFG_A9ROM and the SCFG and MBK range 0x04004000-0x04004063 read 0
  and take no writes, as do the new DMA, camera, DSP, AES, SD/MMC, SDIO Wi-Fi, microphone and
  I2C blocks, IE2/IF2 and the second slot's ports. On the ARM7, SNDEXCNT (0x04004700) and the
  GPIO registers work, as they do on a DSi in this mode.
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
  the firmware user settings, registers and stacks. The ARM9 binary is loaded as the cartridge
  stores it (`arm9.bin`, BLZ-compressed for most games), not decompressed on the host: the
  game's own crt0 decompresses it in place, from the `compressed_end` word of its module
  parameters, which it leaves set. The recompiler works from the decompressed image (the code
  is what crt0 produces; `Rom.arm9` has the word cleared, `Rom.arm9_stored` is the image as
  stored). This is what lets a game restart itself: Pokémon Black 2 reloads its compressed
  program over its own RAM from the cartridge (the ITCM card routine, then a jump to the entry
  at the header's ARM9 address; the module parameters in the uncompressed head stay) after
  quitting the Mystery Gift menu, and its crt0 decompresses it again. When the runtime booted
  a pre-decompressed image with the word cleared, that restart ran crt0 over compressed bytes
  and the ARM9 spun on a destroyed literal pool with a white screen. A crt0 decompresses in
  place, so a compressed image must be safe to decode that way (`blz_compress`, used to build
  test images, makes sure of it and tests/test_rom.py checks it); a decompression routine
  reads the stream's halfwords a byte at a time, since the pointer isn't halfword aligned and
  the ARM9 forces the address even.
- **Sound** (`runtime/sound.c`, ARM7): the 16 channels (PCM8, PCM16, IMA-ADPCM; PSG squares on
  8-13, noise on 14-15), the two capture units and the mixer, as GBATEK "DS Sound" describes
  them. The sound runs lazily, to the machine's time (`Nds.time`, ARM7 bus cycles) at the end of
  each scanline and before every access to its registers, so the busy bit a one-shot clears at
  the start of its last sample, which the SDK polls, is as current as the ARM7's 256-cycle time
  slice. A channel only changes output when its timer overflows, so channels are stepped one at a
  time between the instants something looks at them: a mixed frame or a capture. The mixer keeps
  the hardware's bit widths (volume 16.11, panning 16.8, mixer 20.8, master volume, bias, a
  10-bit clip) and is point-sampled at 32768 Hz of emulated time into a ring of 8192 stereo s16
  frames. `nds_audio_take(s, out, max_frames)` drains it for the app; frames nobody takes are
  dropped, oldest first. Headless `--wav PATH` records every mixed frame to a WAV, finished in
  `nds_shutdown`. The ARM7 BIOS's sound calls are high level too: SWI 08h
  SoundBias, 1Bh GetPitchTable and 1Ch GetVolumeTable. The two tables are generated at first use
  from what they hold, not stored: the pitch table is 2^(i/768) as 16.16 fixed point less 1.0,
  and the volume table is 127 * 10^(dB/200) for a level of (i - 723) tenths of a dB. The SDK's
  driver sets a channel's volume divider from the same level (Div16 below -240, Div4 below -120,
  Div2 below -60), so the table holds the amplitude scaled up by that divider and ramps from 32
  or 64 to 127 in each range. `cpu.c` computes them with its own exp() because difftest and the
  generated build link it without libm. The table tests check the code against these formulas,
  not against a real BIOS.
- **3D engine** (`runtime/gpu3d.c`, `runtime/gpu3d_render.c`; ARM9 0x04000060 and
  0x04000320-0x040006A3). The geometry engine takes commands from GXFIFO (packed words of up to
  four commands, then their parameters) and from the direct ports at 0x04000440-0x040005CC. A
  command runs as soon as its parameters are in, with no latency; its cycle cost only extends how
  long GXSTAT reports busy (box/position/vector tests and stack operations have their own busy
  bits). The FIFO holds entries only while a command waits for parameters and while SWAP_BUFFERS
  halts the engine until VBlank. GXSTAT shows its level (the 4-entry pipe not counted), the
  under-half and empty bits and the FIFO interrupt as a level condition, and geometry FIFO DMA
  (ARM9 timing 7, `nds_gxfifo_dma` in nds.c) sends 112-word bursts only while the FIFO is under
  half full. The queue takes 16384 entries and the CPU never stalls on it. Matrices are 4x4 of
  20.12, row vectors, with the 64-bit accumulate and a single arithmetic shift per element;
  projection and texture stacks are one deep, position and vector stacks 31 with a 6-bit
  pointer, and overflow sets GXSTAT bit 15. The clip matrix (position * projection) is built
  lazily. Polygons are assembled from triangles, quads and strips, culled by facing, clipped
  against all six planes of the view volume (new vertices interpolate colour and texture
  coordinates), put through the viewport, and stored in one of two polygon/vertex RAM sets (2048
  polygons, 6144 vertices; strip vertices are shared until a clip separates them). A polygon
  that clips to more than 10 vertices (a twisted quad) is dropped, since the hardware's
  behaviour for it is undocumented. SWAP_BUFFERS exchanges the sets at VBlank; its sort and W
  bits apply to the commands after the swap. Lighting follows GBATEK but has no tests yet. The
  rasterizer draws the whole frame at line 262 into a 256x192 buffer of 6-bit colour and 5-bit
  alpha with a 24-bit Z or W buffer, scanline by scanline: the rear plane, then opaque polygons
  in the order sent, then translucent ones (sorted by bottom then top Y unless the swap asked for
  manual order; GBATEK gives no sort rule, so that is a guess; polygons with a translucent
  texture, A3I5 or A5I3, count as translucent too, as long as DISP3DCNT bit 0 is on, which is an
  inference from GBATEK's "Translucent Texture" naming: without it a nearer A5I3 polygon sent
  before a farther opaque one is wiped out, because translucent pixels do not write depth). A pixel is drawn when its centre is inside the
  polygon; polygons with no area on screen (lines, dots) draw their whole run, and polygons
  whose bounding box is at most 1x1 draw as one pixel. Colour and W are interpolated
  perspective-correctly, Z linearly. Alpha blending follows the 3D engine's formula, with the
  translucent polygon-ID rule (a translucent pixel does not draw over one with the same ID) and
  bit 11 deciding whether it writes depth; wire frames are alpha 0. Textures and effects are
  described below. The frame is engine A's BG0 when
  DISPCNT bit 3 is set (and POWCNT1 bit 3): BG0HOFS scrolls a 512-pixel region (the image, then
  256 transparent pixels), BG0CNT gives the priority, and with BLDCNT's alpha effect, BG0 as
  first target and a second target below it, a translucent pixel blends by its own alpha (EVA =
  A/2, EVB = 16 - A/2, which GBATEK itself marks as a guess), while alpha 31 stays whole.
- **3D textures and effects** (`gpu3d_render.c`). The frame is drawn in this order: the rear plane
  (CLEAR_COLOR and CLEAR_DEPTH, or, with DISP3DCNT bit 14, a colour bitmap in texture slot 2 and a
  depth bitmap in slot 3, 256x256 and scrolled by CLRIMAGE_OFFSET with wrap), the opaque polygons
  in the order sent, the translucent ones, then edge marking, fog and anti-aliasing as whole-frame
  passes over the colour and attribute buffers. The attribute buffer holds per pixel the opaque and
  translucent polygon IDs, the fog flag, an edge flag, the shadow stencil bit and the
  anti-aliasing coverage. Texture memory is the four 128 KB slots of VRAM banks A-D mapped with
  MST 3 and the eight 16 KB palette slots of banks E, F and G (`nds.c` exports `vram_texslot` and
  `vram_texpal`); a slot no bank covers reads as zero, which is a guess. The renderer fetches one
  texel per pixel at the perspective-correct (S, T), the floor of the 12.12 coordinate, then
  repeat, flip (needs repeat) or clamp per axis, with no filtering. The seven formats follow
  GBATEK's tables: A3I5 (alpha a*4+a/2), 2-, 4- and 8-bit indexed with colour 0 transparent when
  TEXIMAGE_PARAM bit 29 is set (the 2-bit PLTT_BASE steps 8 bytes), 4x4 compressed (blocks in slot
  0 or 2, index halfwords in slot 1, the four palette modes), A5I3 and direct colour. TEXCOORD goes
  through the texture matrix in the four transform modes; the products shift by 20 bits (12+12
  fraction bits down to S's 4). Polygon modes use GBATEK's blend formulas, written for 6-bit colour
  and alpha: 5-bit values become X*2+1 (zero stays zero) and the result is cut back to 5 bits.
  Modulation multiplies texel and vertex colour and alpha. Decal mixes by the texel alpha and keeps
  the polygon's alpha. Toon and highlight take the vertex red as the TOON_TABLE index; highlight
  adds the shading colour back, capped at 63. Shadow polygons: ID 0 only sets a stencil bit where
  the depth test passes and draws nothing; other IDs draw where the stencil is clear, clear it,
  and draw only where their ID differs from the opaque ID beneath. A translucent pixel blends over
  what is there unless that is a translucent pixel of the same polygon ID. Fog uses
  FogDepthBoundary[n] = FOG_OFFSET + (0x400 >> shift) * (n + 1) with linear interpolation of the
  32-entry density table, in colour-and-alpha or alpha-only mode, per polygon by POLYGON_ATTR bit
  15. Edge marking outlines opaque polygons in EDGE_COLOR[id >> 3] where a neighbour has another
  ID and greater depth; the rear plane's ID counts beyond the screen edge. Lighting (NORMAL, four
  lights, DIF_AMB, SPE_EMI, SHININESS) is in `gpu3d.c`; its hardware rounding is unknown, so the
  tests allow one step of the 5-bit result. Anti-aliasing is an approximation: an opaque polygon's
  left and right edge pixels carry a coverage and are mixed with the pixel just outside, or become
  translucent by the coverage over a clear rear plane. `DSN_SANITIZE=1 python3 tests/test_units.py`
  builds the unit tests under ASan and UBSan with the shift checks on.
- **BIOS decompression** (`runtime/cpu.c`): LZ77, run-length and Huffman decoding are one state
  machine that asks for its source one unit at a time and writes bytes, 16-bit pairs (the VRAM
  variants) or words (Huffman). SWIs 11h and 14h run it to the end inside the call. SWIs 12h, 13h
  and 15h (the NDS's callback variants) need guest code, which may wait for hardware, so the job
  is parked in a per-CPU `BiosJob`: `dsr_swi` returns the callback's address with `lr` pointing
  into the BIOS, and when the callback returns there `bios_exec` feeds `r0` to the machine and
  issues the next request, as IntrWait parks at BIOS_INTRWAIT. The 16-bit variants hold the low
  byte until its pair arrives, which reproduces GBATEK's "disp=000h does not work" caveat. Also:
  SoftReset (clears the 200h bytes at the fixed address, sets the three stacks, enters system
  mode with interrupts off, jumps through [027FFE24h]/[027FFE34h]), the Diff8/Diff16 unfilters,
  GetSineTable (round(7FFFh * sin), with a series so `cpu.c` links without libm), CustomPost.
  Not done: GetBootProcs and the DSi-only calls.
- **2D engines** (`runtime/gpu2d.c`), one scanline at a time. Backgrounds: text, affine,
  extended affine (16-bit map entries, and the 256-colour and direct-colour bitmaps) and the large
  512x1024 bitmap (engine A's BG2, mode 6), with BG and OBJ extended palettes from VRAM E-H
  (engine A) and VRAM H-I (engine B). Sprites: regular, affine (double size), bitmap OBJs in both
  mappings with their alpha, OBJ mosaic, semi-transparent OBJs and OBJ window sprites. Windows:
  WIN0, WIN1, OBJ window and outside, with WININ/WINOUT masks and GBATEK's "DS Window Glitches" (X2
  of 0 or below X1 reaches column 256, Y2 past 192 or below Y1 reaches the last line, and Y1 of 1
  to 6 fires again in lines 257 to 262 because the DS compares 8 bits of its line counter). BG
  mosaic repeats the first line and pixel of each block. Effects: alpha blending, brighten and
  darken, master brightness. Engine A's BG0 can be the 3D layer. The display mode is off (white),
  graphics, VRAM display (a whole bank, engine A) or main memory display (the FIFO below).
  BG2X/Y and BG3X/Y are internal reference points: copied at VBlank, advanced by PB/PD each line,
  and a write outside VBlank is copied at the next drawn line, so an HBlank DMA that writes BG2Y
  per line gives the per-line origin of floor and rotation effects. X and Y are independent
  (writing only BGxX leaves Y advancing by PD), and drawing does not depend on the order of
  lines. A change of PB/PD in mid-frame is still taken as if it held since the last copy.
- **Display capture** (`runtime/capture.c`, DISPCAPCNT 0x04000064 and DISP_MMEM_FIFO 0x04000068,
  engine A only). Writing bit 31 requests a capture. The register is latched at line 0 of the
  next frame, and one line is captured per visible line while gpu2d.c composes it. The busy bit
  clears at line 192 whatever the size, which is what games poll. Source A is the engine's
  composed 2D output (before master brightness) or the 3D layer alone (bit 24). Source B is a
  VRAM bank, read through DISPCNT's bank select and the read offset, or the main memory FIFO
  (bit 25). Following GBATEK's block diagram, B reads the display's 256-pixel lines whatever the
  capture size, while the destination is packed `width` pixels a line (a reading of the diagram:
  GBATEK's text does not say it outright). The result is A, B, or the blend `A*EVA + B*EVB`, each
  term counting only where its source pixel is opaque (bit 15), EVA and EVB clamped at 16. Sizes
  are 128x128, 256x64, 256x128 and 256x192; the destination bank and its write offset (steps of
  0x8000) wrap at 128 KB. A capture reads the display's VRAM line before it writes its own, so a
  capture into the bank being shown is the usual one-frame feedback (Pokémon Black 2 blends 12/16
  of the new frame with 4/16 of the old one as motion blur, and shows its 3D scenes this way:
  engine A in VRAM display mode from bank D, capturing the 3D layer into it). Main memory display
  and capture from the FIFO need a DMA channel in the display FIFO timing (4) enabled at line 0:
  `nds.c` then triggers it 32 times a line, 8 pixels a burst, through a 1024-pixel FIFO that each
  frame starts afresh. Capture reads and writes the physical VRAM banks and does not consult
  VRAMCNT.

## Roadmap

### 1. Proof of concept (done)

Recompiler, runtime and the homebrew end-to-end test.

### 2. A commercial game to its title screen

What a Nitro SDK game needs that homebrew doesn't, roughly in the order a game hits it:

- **Save memory**: done (see above). Pokémon Black 2 uses 512 KB of FLASH.
- **RTC**: done (see above). Pokémon's ARM7 uses the SDK's RTC routine, which bit-bangs the
  register with the sequence the unit tests use.
- **Sound**: done (see above): the 16 channels, capture, and mixing to 32768 Hz into a ring for
  the app (`nds_audio_take`), plus the ARM7 BIOS sound calls. The Apple app's AVAudioEngine only
  has to drain the ring.
- **3D**: done (see above) except exact anti-aliasing and texture filtering, and a faster or
  Metal rasterizer (the rasterizer is software and single-threaded).
- **The rest of the 2D engines and display capture**: done (see above). With them Pokémon Black
  2's title scene reaches the screen, textured, and its main menu shows the saved game.
- **Wi-Fi**: done as a stub (see above): the SDK initializes and reports no network.
- **Black 2 play-tested** (headless, on a copy of a real save): the intro movie, the title, Continue into the overworld,
  walking, the X menu (touch), the party screen, saving through the save-chip model (the game writes about 4 KB
  of changes and the file loads again), and a wild battle with the intro camera, the command menu and the
  move list. The enemy in a wild battle looks like a white silhouette only when it is a pale Pokémon (Tynamo); the
  player's Pokémon is shaded dark when seen from behind and its palette is modulated by a slow wave (about 78 frames)
  that the game computes: both were checked against the screens and nothing points at the 3D or 2D engines. Not
  yet exercised: trainer battles, the Pokédex, the Bag, other menus' side effects, Wi-Fi features (stubbed).
- **Game profiles**: per game (by game code and revision), the extra entry points, known
  data-in-code regions and patches. Like nxnative's `dm/games/<id>/`.
- **DSi-enhanced titles** run in DS mode (Black 2 is one); DSi mode is out of scope for now.

**Other games.** All six Pokémon dumps (Black, White, Black 2, White 2, HeartGold, SoulSilver)
recompile, build and boot. They run their start-up, title or notice screens and the first part
of a new game with a blank save (HeartGold and SoulSilver need a touch on NO INFO NEEDED at
(128,153) to get past the first screens; A presses only cycle the notices). Their save traffic
shows the infrared 00h prefix in front of every command, as `save.c` models; Black exercises
05h, 03h, 06h and 0Ah and the 08h probe answered AAh, HeartGold and SoulSilver only 05h and 03h
before their stop, so their write path is untested. Each stopped at the same kind of blocker
before the interpreter fallback: code the game decrypts at run time. An overlay is stored with
the bodies of its functions scrambled (the literal pools between them intact) and RAM differs
from the decompressed image in runs of function bodies (0x64 bytes every 0x78 in HeartGold's
overlay 123). Black's overlay 230 holds the decryptor: a loop over the overlay's words that
finds branches by their opcode (the top byte: B, BL, BLX), adds a constant to the offset,
flips the bit that separates B from BL, and XORs every word with a key that depends on the
words before it. HeartGold's overlay 123 holds trampolines that call a routine and then jump
through ip, so its code is decrypted lazily. Discovery cannot see this code, because it exists
only after the game has run: the compiled bytes are ciphertext, the dispatcher's check bytes
mismatch and the code runs in the interpreter (or, without it, stops with "no recompiled code
at X"). The stops were Black 0x021882a0 (overlay 230, frame 7363), White 0x021882c0, White 2
0x02180ac4 (overlay 337, frame about 7880), Black 2 0x0217c940 (overlay 338, frame about 930 to
1010) and HeartGold/SoulSilver 0x0225f4a8 (overlay 123, frame about 7937). A build-time pass
that applies Black's decryptor to the overlay images would also work, but each game family
needs its own; capturing the decrypted bytes from a run into a local profile is the general
route.

### 3. An Apple app

Built and running for macOS, iOS/iPadOS and tvOS on the homebrew test ROM (no commercial game
built into it yet, nothing run on a physical device). It is a Swift package, `app/`: DSNKit holds
everything shared, DSNCore is a thin C shim, and DSNative is the Mac app; the iPhone/iPad and Apple
TV hosts come from an Xcode project that XcodeGen generates from `app/project.yml`. DSNCore hides
`Nds` from Swift, because its large fixed arrays import as huge tuples. It boots a game as
`main_headless.c` does, copies the two 256x192 0xAARRGGBB framebuffers per frame, and moves
`nds_audio_take`'s output into a lock-free ring that the audio thread drains.

- **Frame loop**: the emulation runs on its own thread, paced to 59.8261 Hz from absolute deadlines
  (it restarts after a stall instead of bursting), and publishes frames through a triple buffer.
- **Screens**: Metal draws the two screens as textured quads from a layout (stacked or side by
  side, aspect-preserving or integer scaling; a TV layout inside Apple TV's overscan margin), and
  the same layout maps a pointer or finger to touch-screen pixels. Shaders are precompiled, with a
  run-time `makeLibrary(source:)` fallback.
- **Input**: keyboard, GameController (with the Siri Remote as a minimal pad on tvOS), mouse or
  touch on the bottom screen, an on-screen pad on iPhone and iPad (it steps aside for a controller
  that has sent input), and a script, all combined.
- **Audio**: an `AVAudioEngine` source node at 32768 Hz stereo, with an `ambient` session on iOS and
  `playback` on tvOS; it restarts after interruptions and device changes.
- **Saves**: Application Support on the Mac and tvOS, Documents on iOS (the Files app shows it),
  written every 10 s when the game changed it, when the app leaves the foreground, and on quit.
  The game pauses in the background.
- **Packaging**: `tools/build_app.py ROM -o OUT --platform {macos,ios,ios-sim,tvos,tvos-sim}`
  recompiles the ROM once, builds `libdsn.a` with each SDK's clang and minimum version (17.0),
  writes an xcconfig, has XcodeGen generate the project, and runs unsigned `xcodebuild`; `--run`
  installs and launches in a simulator and takes a screenshot. A device build needs the user's own
  team and bundle identifier in Xcode. The generated C for a big game is large (about 300 MB of C
  and a 100 MB library for Black 2), and every platform needs its own library, so the C compile
  dominates the build.
- **Tests**: `tests/test_app.py` builds the Mac app on the test ROM, compares its `--smoke` run
  (screens and debug output, and the audio ring against dsn_headless's WAV) with `dsn_headless`
  and runs the package's `swift test`; an iOS-only hit-test test runs through `xcodebuild` on a
  simulator.

Lessons that cost time: a closure made in a main-actor method is itself main-actor isolated and
traps on the audio thread (build the audio source node in a nonisolated function); a SwiftUI view
that ignores the safe area reads zero insets from `GeometryProxy`; and the iOS Simulator lists a
stand-in gamepad.

### 4. Speed

Measure first. `tools/bench.py` runs `dsn_headless` several times from the same start (a fresh
copy of the save, `--rtc` fixed) and reports emulated frames per second as the best frames of the
runs, per window of frames, with the median, 99th-percentile and worst frame time against the
16.72 ms a frame may take (59.8261 fps) and 4.18 ms at 4x real time. The machine may be shared
with other work, so every frame takes the minimum over the runs and `--vs DIR` alternates the runs
of two builds. `--profile A:B` samples the emulation thread (macOS: a helper thread suspends it
every millisecond and reads pc, lr and the frame-pointer chain; a SIGPROF timer would be skewed
towards system calls on XNU) and sorts the samples into the machine's parts. It samples only the
emulation thread, so with render threads the rasterizer's share is the part the emulation thread
ran or waited for; use `--render-threads 1` for an attribution. `--hash` makes `dsn_headless`
print hashes of both screens, all memory, both CPUs' registers and cycle counts, and the audio at
chosen frames: a change that must not alter the emulation leaves them equal, and `--expect FILE` /
`--vs` check that. `--workload` runs the test ROM's CPU benchmark (`tests/hw/bench_kernels.h`:
CRC, quicksort, recursion, matrix multiply, copies, a jump-table VM, hardware registers; started
by holding START, checksums checked against a host build of the same source). `--sample` runs
macOS's `sample` on a live run as a rough cross-check.

What the measurements said (Apple M6, 4 render threads, Pokémon Black 2 from a save copy with A
pressed every 100 frames to frame 900): before the speed work the boot, logos and intro (frames
0-600) cost 1.0 ms a frame and the 3D title scene and main menu (600-900) 2.3-2.5 ms, about 7x
real time, worst frame 3.3 ms; the game's CPUs are almost idle there. In the title scene (one
render thread) the 3D rasterizer is about 53% of the time, the 2D renderer 16%, the geometry
engine 9%, recompiled ARM9/ARM7 code 6%, the dispatcher 2%, the memory slow path and I/O registers
4%. So the work went into the renderers:

- The 2D renderer takes the engines' VRAM block tables once a line (a mask and a load per access
  instead of a call and a modulo per pixel), fetches one map entry and one tile row per 8 pixels,
  walks a list of layers in drawing order and stops at the first opaque layer unless a blend can
  use the one behind it, and paints lines that have no window, blend or semi-transparent sprite
  back to front in plain vectorizable passes. No arithmetic changed.
- The 3D rasterizer draws the frame as 12 bands of 16 rows on a small worker pool
  (`runtime/workers.c`: one mutex, helpers started at the first run, the caller takes jobs too;
  the default is the number of cores, at most 4; `--render-threads N`, 1 for none). A pixel depends
  only on the polygons that cover it, in the same order whatever the rows around it, so the frame
  is identical for any thread count (`tests/unit/test_threads.c`); edge marking, fog and
  anti-aliasing still run over the whole frame after the join. Frames of fewer than 48 polygons
  are drawn in the caller. `span()` does the depth test before the perspective factor and
  attribute interpolation (58% of the pixels Black 2's title scene visits fail it). Helper threads
  inherit the QoS class of the thread that first calls the rasterizer, so the app's emulation
  thread decides it. The pool serves one emulation thread at a time: two machines drawing
  concurrently in one process would share it.
- The dispatcher reads the machine's IME, IE and IF in place (no call through a pointer at every
  dispatch) and the cache line holds the function. The interrupt is taken at exactly the same
  dispatches as before.

Result: all 900 frames 1.418 -> 0.643 ms a frame (2.2x); the 3D title scene and menu 1.09-1.18 ms
(14-15x real time; 1.56-1.98 ms, 9-11x, on one render thread), the rest 0.33-0.46 ms (36-50x);
bit-identical output at every step. Checked again at the merge on a Black 2 session with the
real save (title, menu, overworld, a 3D battle at frame 3700, hashes of screens, memory, CPU
state and audio at seven frames): identical to the build before the optimizations with 1, 4 and
the default number of render threads, and 8.9 s against 4.7 s of wall time for the 3700 frames.

Not done, because the numbers do not justify it yet: guest registers in host locals, flag
liveness, direct calls for static BL targets and inline memory access fast paths (the last is
already inline: `rd32`/`wr32` are page-table lookups). A fully busy ARM9 (1,120,380 guest cycles a
frame) costs 0.21-0.57 ms a frame including screen rendering on the CPU benchmark's kernels, at
least 29x real time, and about 12% of Black 2's title-scene frame is guest code, dispatcher and
slow paths together. Direct calls would also have to keep the interrupt check at each call (the
dispatcher takes interrupts at every dispatch, and cycle counts must not move) and bound the host
stack. The hardware-register kernel is the slowest (1.84 ms a frame at 100% busy, 9x real time):
every I/O access goes through a chain of eight subsystem hooks (`sub_io_read`/`sub_io_write`); a
range prefilter is the next thing to try there. The ARM9's cycle counts are still one per
instruction, so cycle counts per instruction class remain for games that depend on timing.

Build cost (the generated code is unchanged by this work): Black 2 recompiles in 39 s into 241 MB
of C in 179 files; `make -j4` at -O2 takes about 100 s wall and 370-390 s of CPU (135 MB of
objects, a 120 MB executable). The optimizer is the cost: a 0.9 MB generated file parses in 0.03
s, compiles in 0.16 s at -O0 and in 1.1-1.2 s at -O1, -Os and -O2, so -O1 and -Os save nothing.
-O0 builds the whole game in 15 s (49 s of CPU) and runs Black 2 at 3.35 ms a frame (2.9x real
time in the 3D scenes), bit-identical: `make CFLAGS=-O0` is the development build. Compiling only
the functions a game profile finds hot at -O2 and the rest at -O0 is the obvious next step for
build time.

Real time for the target: 59.8261 fps with margin; 4x on the Mac (4.18 ms a frame) is the goal
for Black 2's title scene and menu. The baseline already met it on average (6.4-7.3x); the worst
reachable frames were 3.3 ms and are now 1.7 ms with threads (2.6 ms on one thread). Nothing was
run on a device.

### 5. 3DS

The same recompiler grows to ARMv6K (the ARM11 MPCore: more instructions, VFPv2, the
multiprocessor primitives). The runtime grows a Horizon layer: the kernel's SVCs and the
services `gsp`, `hid`, `fs`, `apt`, `dsp` and `ndm`, implemented at a high level as nxnative
does for the Switch. PICA200 shaders get translated to Metal ahead of time, and the
fixed-function TEV stages become generated fragment shaders.
