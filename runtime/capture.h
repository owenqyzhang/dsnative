/* dsnative runtime: engine A's display capture and main memory display FIFO (GBATEK "DS Video
 * Capture and Main Memory Display Mode"): DISPCAPCNT (0x04000064) and DISP_MMEM_FIFO
 * (0x04000068). gpu2d.c composes the lines and calls capture_line for each one a capture covers. */
#ifndef CAPTURE_H
#define CAPTURE_H
#include "dsr.h"

struct Nds;

#define CAPTURE_FIFO 1024           /* pixels; a line is 256 */

typedef struct {
    u32 cnt;                        /* DISPCAPCNT as written; bit 31 (busy) clears when the capture ends */
    u32 run;                        /* the register as latched at line 0 for the capture of this frame, 0 if none */
    int dma_ok;                     /* a main memory display DMA channel was enabled at line 0: it feeds this frame */
    u16 fifo[CAPTURE_FIFO];         /* the display FIFO: pixels the DMA has sent that no line has taken yet */
    u32 fifo_head, fifo_count;
} Capture;

/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int capture_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int capture_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* Start of scanline `line`: a capture requested since the last frame begins at line 0 and its
 * busy bit clears at line 192, whatever its size. */
void capture_line_start(struct Nds *s, u32 line);
/* Does the display FIFO need its DMA (timing 4) this line: engine A shows main memory, or a
 * capture takes its source B from it. nds.c then runs 32 bursts of 8 pixels. */
int capture_fifo_wanted(struct Nds *s);

/* For gpu2d.c, engine A, visible line `line`: */
int capture_covers(struct Nds *s, int line);            /* a capture writes this line */
int capture_wants_graphics(struct Nds *s);              /* ... and needs the composed 2D output (source A = 0) */
int capture_wants_fifo(struct Nds *s);                  /* ... and needs the FIFO's pixels */
/* Takes the 256 pixels of this line from the FIFO (black where the DMA has sent too few). */
void capture_fifo_take(struct Nds *s, u16 *px);
/* Writes the line: gfx is the composed 2D output as 15-bit colours before master brightness (read
 * when source A is the graphics screen), fifo the FIFO's pixels, vram_display is DISPCNT's VRAM
 * display mode (which ignores the read offset). The display has read its own VRAM line already. */
void capture_line(struct Nds *s, int line, const u16 *gfx, const u16 *fifo, int vram_display);
#endif
