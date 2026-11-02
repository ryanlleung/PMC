#include "pmc_config.h"

// USB build only: in the Ethernet build link.c uses ethlink.c, and any
// TinyUSB reference here would pull in the whole USB stack (see
// tinyusb_cdc_device.c).
#if !PMC_LINK_ETHERNET

#include <stdarg.h>
#include "lvgl.h"  // lv_vsnprintf: avoids pulling in newlib printf
#include <string.h>

#include "mcu.h"
#include "usb_hw.h"
#include "tusb.h"
#include "usb_serial.h"
#include "link.h"

/* --------------------------------------------------------------------------
 * Clock
 *
 * The NECTO setup runs the PLL from HSI with PLLQ = 9, which gives the USB
 * peripheral 336 / 9 = 37.3 MHz. USB full speed needs exactly 48 MHz.
 * Rebuild the PLL as HSE 25 MHz (X3 on the board schematic) / 25 * 336,
 * P = 2 (SYSCLK 168 MHz, as now), Q = 7 (48 MHz). If HSE does not start,
 * stay on HSI 16 MHz / 16 with Q = 7 (48 MHz, but HSI is +/-1 %, outside
 * the USB spec, so may be unreliable).
 * ------------------------------------------------------------------------ */
#define HSE_STARTUP_TIMEOUT 500000UL

static void usb_clock_48mhz(void)
{
    uint32_t pll_src = 0;    // HSI
    uint32_t pll_m = 16;     // 16 MHz HSI -> 1 MHz
    uint32_t timeout = HSE_STARTUP_TIMEOUT;

    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY) && --timeout);
    if (RCC->CR & RCC_CR_HSERDY)
    {
        pll_src = RCC_PLLCFGR_PLLSRC_HSE;
        pll_m = 25;          // 25 MHz HSE -> 1 MHz
    }
    else
        RCC->CR &= ~RCC_CR_HSEON;

    // Run from HSI while the PLL is reconfigured.
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI);

    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY);

    RCC->PLLCFGR = pll_src
                 | (pll_m << RCC_PLLCFGR_PLLM_Pos)   // 1 MHz PLL input
                 | (336UL << RCC_PLLCFGR_PLLN_Pos)   // 336 MHz VCO
                 | (0UL   << RCC_PLLCFGR_PLLP_Pos)   // /2 -> 168 MHz SYSCLK
                 | (7UL   << RCC_PLLCFGR_PLLQ_Pos);  // /7 -> 48 MHz USB

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);
}

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */
void usb_serial_init(void)
{
    usb_clock_48mhz();
    usb_hw_init();
    // NECTO's terminal opens COM3 without DTR. TinyUSB's default then lets a
    // full TX FIFO overwrite its oldest bytes, which dropped the start of
    // replies longer than the FIFO (PID? on the bench, 26 Oct). The FIFO is
    // kept from overwriting; usb_serial_printf waits for room instead.
    static const tud_cdc_configure_t cdc_cfg = {
        .rx_persistent = 0, .tx_persistent = 0, .tx_overwritabe_if_not_connected = 0 };
    tud_cdc_configure(&cdc_cfg);
    tusb_init(0, NULL);
}

void usb_serial_task(void)
{
    tud_task();
}

const char *usb_serial_status(void)
{
    if (!tud_mounted())
        return "USB: not enumerated";
    if (!tud_cdc_connected())
        return "USB: enumerated, port closed (no DTR)";
    return "USB: port open";
}

size_t usb_serial_tx_room(void)
{
    if (!tud_mounted())
        return (size_t)-1;
    return tud_cdc_write_available();
}

void usb_serial_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    int len;

    // Only needs the PC to have enumerated the device. Do not wait for DTR:
    // some terminals open the port without asserting it.
    if (!tud_mounted())
        return;

    va_start(args, fmt);
    len = lv_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len <= 0)
        return;
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;

    // The CDC TX FIFO (tinyusb_cdc_device.c, 256 bytes) can be full, so a longer
    // line has to be fed in pieces while the stack sends. Give up after
    // ~10 ms if the PC is not reading (no terminal open), dropping the rest.
    // After that, lines that do not fit are dropped whole without waiting,
    // until the PC reads again, so a closed port costs the loop nothing.
    static bool stuck;
    if (stuck && tud_cdc_write_available() < (uint32_t)len)
        return;
    stuck = false;
    uint32_t sent = 0;
    uint32_t t0 = lv_tick_get();
    while (sent < (uint32_t)len) {
        uint32_t n = tud_cdc_write(buf + sent, (uint32_t)len - sent);
        sent += n;
        tud_cdc_write_flush();
        if (n == 0) {
            tud_task();
            if (lv_tick_elaps(t0) > 10) {
                stuck = true;
                break;
            }
        }
    }
}

#define LINE_IDLE_MS 200
// Nothing received for this long after a character, then more of the same
// line: it is being typed key by key (a terminal such as PuTTY), not sent
// as one text.
#define TYPED_GAP_MS 50
// A key-by-key line left this long without CR or LF is dropped (not run),
// so a terminal that sends no line ending (NECTO) is not stuck behind it.
#define TYPED_FORGET_MS 10000

static uint32_t rx_bytes;

uint32_t usb_serial_rx_bytes(void)
{
    return rx_bytes;
}

bool usb_serial_getline(char *buf, size_t n)
{
    static link_line_t line;
    static bool typed;        // the pending line came key by key
    static uint32_t quiet_ms; // the FIFO seen empty this long after the line's last character

    while (tud_cdc_available()) {
        int32_t c = tud_cdc_read_char();
        if (c < 0)
            break;
        rx_bytes++;
        // Measured from when the FIFO was seen empty, not from when this
        // character is read: a packet that waited through a long loop pass
        // (the NECTO terminal's second 64 bytes) is not a pause.
        if (line.len > 0 && quiet_ms > TYPED_GAP_MS)
            typed = true;
        quiet_ms = 0;
        if (link_line_feed(&line, (char)c, buf, n)) {
            typed = false;
            return true;
        }
        if (line.len == 0)
            typed = false;   // CR or LF on an empty line
    }

    // The NECTO UART Terminal sends the text with no line ending, so a line
    // also ends after LINE_IDLE_MS with nothing more received. Not a line
    // typed key by key: a pause there would run half a command (VALV SET 5
    // for 50), so it waits for CR or LF.
    if (line.len > 0)
        quiet_ms = lv_tick_elaps(line.last_ms);   // the FIFO is empty here
    if (typed) {
        if (quiet_ms > TYPED_FORGET_MS) {
            line.len = 0;
            line.over = false;
            typed = false;
        }
        return false;
    }
    return link_line_idle(&line, LINE_IDLE_MS, buf, n);
}

/* --------------------------------------------------------------------------
 * Descriptors: one CDC ACM interface.
 * ------------------------------------------------------------------------ */
#define USB_VID 0xCAFE  // TinyUSB test VID, fine for bench use
#define USB_PID 0x4001

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *string_desc[] = {
    "",                 // 0: language (handled below)
    "MikroElektronika", // 1: manufacturer
    "PMC Mikromedia 4", // 2: product
    "0001",             // 3: serial
    "PMC Serial",       // 4: CDC interface
};

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == 0) {
        desc_str[1] = 0x0409;  // English (US)
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc) / sizeof(string_desc[0]))
            return NULL;
        const char *str = string_desc[index];
        chr_count = strlen(str);
        if (chr_count > 32)
            chr_count = 32;
        for (size_t i = 0; i < chr_count; i++)
            desc_str[1 + i] = str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}

#endif // !PMC_LINK_ETHERNET
