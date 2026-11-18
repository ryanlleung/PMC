#include "mcu.h"
#include "lvgl.h"  // lv_tick_get for timeouts
#include "extflash.h"

#define SF_CS   11  // PB11
#define SF_SCK  13  // PB13
#define SF_MISO 14  // PB14
#define SF_MOSI 15  // PB15

#define CMD_WREN    0x06
#define CMD_RDSR    0x05
#define CMD_READ    0x03
#define CMD_PP      0x02
#define CMD_SE4K    0x20
#define CMD_ULBPR   0x98
#define CMD_JEDEC   0x9F

static uint32_t jedec_id;

static void sf_delay(void)
{
    for (volatile int i = 0; i < 4; i++);
}

static void cs(int level)
{
    GPIOB->BSRR = level ? (1UL << SF_CS) : (1UL << (SF_CS + 16));
    sf_delay();
}

static uint8_t xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        GPIOB->BSRR = ((out >> i) & 1) ? (1UL << SF_MOSI) : (1UL << (SF_MOSI + 16));
        sf_delay();
        GPIOB->BSRR = 1UL << SF_SCK;            // device samples on rising edge
        in = (uint8_t)((in << 1) | ((GPIOB->IDR >> SF_MISO) & 1));
        sf_delay();
        GPIOB->BSRR = 1UL << (SF_SCK + 16);
    }
    return in;
}

static void cmd_addr(uint8_t cmd, uint32_t addr)
{
    xfer(cmd);
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
}

static void write_enable(void)
{
    cs(0); xfer(CMD_WREN); cs(1);
}

static bool wait_ready(uint32_t timeout_ms)
{
    uint32_t t0 = lv_tick_get();
    for (;;) {
        cs(0); xfer(CMD_RDSR); uint8_t sr = xfer(0); cs(1);
        if (!(sr & 0x01))
            return true;
        if (lv_tick_elaps(t0) > timeout_ms)
            return false;
    }
}

static void out_pin(GPIO_TypeDef *port, int pin, int level)
{
    port->BSRR = level ? (1UL << pin) : (1UL << (pin + 16));
    port->OTYPER &= ~(1UL << pin);
    port->MODER = (port->MODER & ~(3UL << (2 * pin))) | (1UL << (2 * pin));
}

bool extflash_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOGEN;

    // Other devices on SPI2 deselected.
    out_pin(GPIOG, 9, 1);    // nRF CS
    out_pin(GPIOD, 11, 1);   // MP3 CS
    out_pin(GPIOD, 10, 1);   // MP3 DCS

    out_pin(GPIOB, SF_CS, 1);
    out_pin(GPIOB, SF_SCK, 0);
    out_pin(GPIOB, SF_MOSI, 0);
    GPIOB->MODER &= ~(3UL << (2 * SF_MISO));  // input

    cs(0);
    xfer(CMD_JEDEC);
    jedec_id = (uint32_t)xfer(0) << 16;
    jedec_id |= (uint32_t)xfer(0) << 8;
    jedec_id |= xfer(0);
    cs(1);

    if ((jedec_id >> 16) != 0xBF)  // Microchip/SST
        return false;

    // SST26 powers up with every block write-protected.
    write_enable();
    cs(0); xfer(CMD_ULBPR); cs(1);
    return true;
}

uint32_t extflash_jedec_id(void)
{
    return jedec_id;
}

void extflash_read(uint32_t addr, void *buf, size_t len)
{
    uint8_t *p = buf;
    cs(0);
    cmd_addr(CMD_READ, addr);
    while (len--)
        *p++ = xfer(0);
    cs(1);
}

bool extflash_erase_4k(uint32_t addr)
{
    write_enable();
    cs(0); cmd_addr(CMD_SE4K, addr & ~0xFFFUL); cs(1);
    return wait_ready(100);   // 25 ms max per datasheet class; margin
}

bool extflash_write(uint32_t addr, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        size_t n = 256 - (addr & 0xFF);
        if (n > len)
            n = len;
        write_enable();
        cs(0);
        cmd_addr(CMD_PP, addr);
        for (size_t i = 0; i < n; i++)
            xfer(p[i]);
        cs(1);
        if (!wait_ready(10))
            return false;
        addr += n; p += n; len -= n;
    }
    return true;
}
