/* The touch screen on the ARM7's SPI bus (device 2): the ADC reads for a stylus at each screen
 * pixel, driven the way the SDK's touch code does (a control byte, then two data bytes with the
 * bus held). The firmware's calibration maps ADC 0x200 at pixel 0x1F to 0xE00 at 0xDF
 * horizontally and to 0x800 at 0x9F vertically (GBATEK "DS Touch Screen Controller"). */
#include <stdio.h>
#include <stdlib.h>
#include "nds.h"

static Nds *s;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { CH_Y = 1, CH_X = 5 };

static u8 xfer(u8 b, int hold) {
    wr16(&s->arm7, 0x040001C0, (u16)(0x8000 | (2 << 8) | (hold ? 0x800 : 0)));
    wr8(&s->arm7, 0x040001C2, b);
    return (u8)rd8(&s->arm7, 0x040001C2);
}

/* one 12-bit conversion of a channel */
static u32 adc(int ch) {
    xfer((u8)(0x80 | ch << 4), 1);
    u8 hi = xfer(0, 1), lo = xfer(0, 0);
    CHECK(!(hi & 0x80), "result byte 1 is %02x, its top bit is the unused one", hi);
    return (u32)hi << 5 | lo >> 3;
}

static u32 read_x(int px) { s->touch_down = 1; s->touch_x = px; s->touch_y = 0; return adc(CH_X); }
static u32 read_y(int py) { s->touch_down = 1; s->touch_x = 0; s->touch_y = py; return adc(CH_Y); }

/* the calibration's straight line, in signed arithmetic (pixels below 0x1F are left of it) */
static int line_x(int px) { return 0x200 + (px - 0x1F) * ((0xE00 - 0x200) / (0xE0 - 0x20)); }
static int line_y(int py) { return 0x200 + (py - 0x1F) * ((0x800 - 0x200) / (0xA0 - 0x20)); }

int main(void) {
    s = nds_create(calloc(1, 0x1000), 0x1000);

    /* the calibration points themselves */
    CHECK(read_x(0x1F) == 0x200, "x 0x1F: %03x", read_x(0x1F));
    CHECK(read_x(0xDF) == 0xE00, "x 0xDF: %03x", read_x(0xDF));
    CHECK(read_y(0x1F) == 0x200, "y 0x1F: %03x", read_y(0x1F));
    CHECK(read_y(0x9F) == 0x800, "y 0x9F: %03x", read_y(0x9F));

    /* the first 31 pixels lie left of and above the calibration points: the line goes on to
     * smaller values (0x010 at x 0), it does not wrap round to a large one */
    static const int xs[] = {0, 1, 2, 15, 30, 31, 32, 100, 223, 224, 254};
    for (unsigned i = 0; i < sizeof xs / sizeof *xs; i++)
        CHECK(read_x(xs[i]) == (u32)line_x(xs[i]), "x %d: %03x, expected %03x", xs[i], read_x(xs[i]), line_x(xs[i]));
    static const int ys[] = {0, 1, 2, 15, 30, 31, 32, 100, 159, 160, 190, 191};
    for (unsigned i = 0; i < sizeof ys / sizeof *ys; i++)
        CHECK(read_y(ys[i]) == (u32)line_y(ys[i]), "y %d: %03x, expected %03x", ys[i], read_y(ys[i]), line_y(ys[i]));

    /* the last column: the line reaches 0x1000, which the 12-bit converter clips at 0xFFF
     * (it must not wrap to 0, the far left) */
    CHECK(read_x(255) == 0xFFF, "x 255: %03x", read_x(255));

    /* and no step backwards anywhere on the screen */
    u32 prev = 0;
    for (int px = 0; px < 256; px++) {
        u32 v = read_x(px);
        CHECK(v >= prev, "x %d reads %03x after %03x", px, v, prev);
        CHECK(v <= 0xFFF, "x %d reads %03x, beyond 12 bits", px, v);
        prev = v;
    }
    prev = 0;
    for (int py = 0; py < 192; py++) {
        u32 v = read_y(py);
        CHECK(v >= prev, "y %d reads %03x after %03x", py, v, prev);
        CHECK(v <= 0xFFF, "y %d reads %03x, beyond 12 bits", py, v);
        prev = v;
    }

    /* a position outside the screen (the headless --touch script does not clamp) stays 12 bits */
    CHECK(read_x(-5) == 0, "x -5: %03x", read_x(-5));
    CHECK(read_x(300) == 0xFFF, "x 300: %03x", read_x(300));
    CHECK(read_y(-20) == 0, "y -20: %03x", read_y(-20));        /* the line is 0x200 - 51 * 12 < 0 */
    CHECK(read_y(400) == 0xFFF, "y 400: %03x", read_y(400));

    /* no stylus: X reads 0 and Y 0xFFF, whatever the last position was */
    s->touch_down = 0; s->touch_x = 100; s->touch_y = 100;
    CHECK(adc(CH_X) == 0 && adc(CH_Y) == 0xFFF, "pen up: x %03x, y %03x", adc(CH_X), adc(CH_Y));

    printf(failures ? "touch: %d failures\n" : "touch ok\n", failures);
    return failures != 0;
}
