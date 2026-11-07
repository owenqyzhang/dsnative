/* Save memory test (runtime/save.c): the ARM9 takes the NDS slot (EXMEMCNT bit 11 clear) and
 * drives the FLASH that expect_save.txt picks with the commands the Nitro SDK uses: RDID,
 * RDSR, WREN, page erase, page write, page program, READ and FAST READ. Then it lends the slot
 * to the ARM7, which reads the page back. Prints:
 *   save <JEDEC ID> <status: at start, after WREN, after the erase> <hash of the page> ok
 *   save7 <the ARM7's hash of the page>
 *   savein <hash of what the page held before>  (the save file's: tests/test_save.py checks it) */
#include "save_spi.h"

#define PAGE 0x7F00u

static u8 status(void) { spi_byte(0x05, 1); return spi_byte(0, 0); }
static void wren(void) { spi_byte(0x06, 0); }
static void wait_done(void) { while (status() & 1) ; }     /* WIP */

void save_test9(void) {
    REG_EXMEMCNT &= ~0x0800;
    spi_byte(0x9F, 1);
    u32 id = (u32)spi_byte(0, 1) << 16;
    id |= (u32)spi_byte(0, 1) << 8;
    id |= spi_byte(0, 0);
    u32 sr = (u32)status() << 16;
    u32 before = spi_read_hash(0x03, PAGE, 256);
    wren();
    sr |= (u32)status() << 8;
    spi_cmd_addr(0xDB, PAGE + 0x80, 0);                     /* page erase: the page holding it */
    wait_done();
    sr |= status();                                         /* WEL clears when it is done */
    u32 ff = 0x811C9DC5u;
    for (int i = 0; i < 256; i++) ff = fnv_byte(ff, 0xFF);
    int ok = spi_read_hash(0x03, PAGE, 256) == ff;
    wren();
    spi_cmd_addr(0x0A, PAGE, 1);                            /* page write: replaces bytes */
    for (int i = 0; i < 256; i++) spi_byte((u8)(i * 37 + 11), i < 255);
    wait_done();
    wren();
    spi_cmd_addr(0x02, PAGE + 0x10, 1);                     /* page program: clears bits only */
    for (int i = 0; i < 16; i++) spi_byte(0x0F, i < 15);
    wait_done();
    u32 sum = spi_read_hash(0x03, PAGE, 256);
    ok &= spi_read_hash(0x0B, PAGE, 256) == sum;
    REG_EXMEMCNT |= 0x0800;                                 /* the ARM7's turn */
    u32 sum7 = arm7_command(CMD7_SAVE, PAGE);
    REG_EXMEMCNT &= ~0x0800;
    dbg("save "); dbg_hex(id); dbg(" "); dbg_hex(sr); dbg(" "); dbg_hex(sum); dbg(ok ? " ok\n" : " FAIL\n");
    dbg("save7 "); dbg_hex(sum7); dbg("\n");
    dbg("savein "); dbg_hex(before); dbg("\n");
}
