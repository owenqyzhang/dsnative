/* dsnative runtime: the cartridge's save memory (EEPROM, FLASH or FRAM) on the AUXSPI bus
 * (0x040001A0-0x040001A3), kept in a file on disk. */
#ifndef SAVE_H
#define SAVE_H
#include "dsr.h"

struct Nds;

/* chip types, in --save-type's names: none, eeprom512, eeprom8k, eeprom64k, eeprom128k,
 * flash256k, flash512k, flash1m, flash8m, fram32k */
enum { SAVE_NONE, SAVE_EEPROM512, SAVE_EEPROM8K, SAVE_EEPROM64K, SAVE_EEPROM128K, SAVE_FLASH256K,
       SAVE_FLASH512K, SAVE_FLASH1M, SAVE_FLASH8M, SAVE_FRAM32K, SAVE_TYPES };

typedef struct {
    /* set up at the chip's first use, once the options are in */
    int type;                   /* SAVE_*: --save-type, else the game code, the file's size, the default */
    int type_set, ready;
    int ir;                     /* infrared cartridge: save commands come after a 00h prefix */
    u8 *mem; u32 size;
    char *path; int dirty;      /* --save: read at setup, written back at shutdown if changed */
    u8 *tail; u32 tail_len;     /* what the file holds past the chip's size, written back as is */
    /* the bus */
    u16 cnt;                    /* AUXSPICNT (mirrored in Nds.auxspicnt for the card ROM code) */
    u8 data;                    /* AUXSPIDATA: the byte last received */
    int selected;               /* chip select is low: the bytes belong to one command */
    /* the chip */
    u8 cmd, wel, status, asleep;   /* status: the write-protect bits of EEPROM and FRAM */
    u32 pos, addr;              /* bytes of the command so far, the address it has reached */
    u8 latch[256], latched[256];   /* page writes wait here until chip select rises */
    u32 latch_base;
    u8 ir_cmd; u32 ir_pos;      /* the infrared chip's command */
} Save;

void save_init(struct Nds *s);
/* An I/O access (offset from 0x04000000, size 1, 2 or 4) this subsystem handles: returns 1
 * (with *v for a read), or 0 to leave it to nds.c. */
int save_io_read(struct Nds *s, int cpu, u32 off, int size, u32 *v);
int save_io_write(struct Nds *s, int cpu, u32 off, int size, u32 v);
/* A command-line style option ("--name", "value"): returns 1 if it is this subsystem's.
 * --save-type TYPE (refused when TYPE is unknown) and --save PATH. */
int save_option(struct Nds *s, const char *name, const char *value);
/* Writes the save file now if the memory changed since it was read or last written (an app
 * calls this, with the machine paused, when it goes to the background); returns 0 on success. */
int save_flush(struct Nds *s);
void save_shutdown(struct Nds *s);
#endif
