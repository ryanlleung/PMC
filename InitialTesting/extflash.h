#ifndef _EXTFLASH_H_
#define _EXTFLASH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief On-board serial flash (U6, SST26VF064B, 8 MB) on SPI2:
 * CS PB11, SCK PB13, MISO PB14, MOSI PB15. Bit-banged, mode 0.
 *
 * Kept separate from the MCU's own flash so neither the application nor
 * mikroBootloader can erase what is stored here when the board is
 * reflashed. The same SPI2 lines go to the nRF (CS PG9) and MP3 (CS PD11,
 * DCS PD10) parts; their selects are held high.
 */

// Reads the JEDEC ID and unlocks block protection. True if an SST26 answered.
bool extflash_init(void);

// JEDEC ID as read by extflash_init(), e.g. 0xBF2643.
uint32_t extflash_jedec_id(void);

void extflash_read(uint32_t addr, void *buf, size_t len);

// Erases the 4 KB sector containing addr.
bool extflash_erase_4k(uint32_t addr);

// Programs len bytes (erase first); splits at 256-byte page boundaries.
bool extflash_write(uint32_t addr, const void *buf, size_t len);

#endif // _EXTFLASH_H_
