#include "mcu.h"
#include "pmc_config.h"
#include "eth_hw.h"

#if PMC_LINK_ETHERNET

/* --------------------------------------------------------------------------
 * Board wiring (Mikromedia 4 STM32F4 schematic): LAN8720A in RMII mode,
 * 50 MHz reference clock from MCU MCO1 (PA8) to the PHY and back into
 * ETH_RMII_REF_CLK (PA1).
 *   PA1 REF_CLK, PA2 MDIO, PA7 CRS_DV, PC1 MDC, PC4 RXD0, PC5 RXD1,
 *   PG11 TX_EN, PG13 TXD0, PG14 TXD1, PG6 INT# (unused), PG8 RST#.
 * These differ from the SDK's generic hw_eth.h (TX on PB11-PB13), which
 * would also take the serial flash CS (PB11), so it is not used.
 *
 * Clock: RMII needs exactly 50 MHz and MCO1 can only divide the main PLL
 * (or HSE, 25 MHz). 168 MHz / 3 = 56 MHz is out of spec, so the PLL is run
 * at HSE 25 / 25 * 300 = 300 MHz VCO, SYSCLK 150 MHz, MCO1 = 150 / 3 = 50 MHz.
 * 50 MHz and USB's 48 MHz cannot both come from one VCO (<= 432 MHz), so
 * the USB COM port is off in the Ethernet build.
 * ------------------------------------------------------------------------ */

#define SYSCLK_HZ 150000000UL

// ETH registers used before the stack takes over (RM0090 33.8).
#define ETH_BASE_ADDR   0x40028000UL
#define ETH_MACMIIAR    (*(volatile uint32_t *)(ETH_BASE_ADDR + 0x0010))
#define ETH_MACMIIDR    (*(volatile uint32_t *)(ETH_BASE_ADDR + 0x0014))
#define ETH_DMABMR      (*(volatile uint32_t *)(ETH_BASE_ADDR + 0x1000))
#define MIIAR_MB        (1UL << 0)
#define MIIAR_CR_DIV102 (4UL << 2)   // HCLK 150-168 MHz -> MDC ~1.5 MHz
#define DMABMR_SR       (1UL << 0)
#define AHB1ENR_ETHMAC  ((1UL << 25) | (1UL << 26) | (1UL << 27))
#define AHB1RSTR_ETHMAC (1UL << 25)
#define APB2ENR_SYSCFG  (1UL << 14)
#define SYSCFG_PMC_ADDR (*(volatile uint32_t *)(0x40013800UL + 0x04))
#define PMC_RMII_SEL    (1UL << 23)
#define SYST_RVR        (*(volatile uint32_t *)0xE000E014UL)
#define SYST_CVR        (*(volatile uint32_t *)0xE000E018UL)

static void spin(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++);
}

static void pin_af(GPIO_TypeDef *port, int pin, int af)
{
    port->OSPEEDR |= 3UL << (2 * pin);
    port->OTYPER &= ~(1UL << pin);
    port->PUPDR &= ~(3UL << (2 * pin));
    port->AFR[pin >> 3] = (port->AFR[pin >> 3] & ~(0xFUL << (4 * (pin & 7)))) | ((uint32_t)af << (4 * (pin & 7)));
    port->MODER = (port->MODER & ~(3UL << (2 * pin))) | (2UL << (2 * pin));
}

bool eth_hw_clock_50mhz(void)
{
    uint32_t timeout = 500000;

    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY) && --timeout);
    if (!(RCC->CR & RCC_CR_HSERDY))
        return false;    // HSI is +/-1 %, too far off for RMII

    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY));
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI);

    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY);
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSE
                 | (25UL  << RCC_PLLCFGR_PLLM_Pos)   // 1 MHz PLL input
                 | (300UL << RCC_PLLCFGR_PLLN_Pos)   // 300 MHz VCO
                 | (0UL   << RCC_PLLCFGR_PLLP_Pos)   // /2 -> 150 MHz SYSCLK
                 | (7UL   << RCC_PLLCFGR_PLLQ_Pos);  // 42.9 MHz, USB unused
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY));
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);

    // MCO1 = PLL / 3 = 50 MHz on PA8.
    RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_MCO1_Msk | RCC_CFGR_MCO1PRE_Msk))
              | (3UL << RCC_CFGR_MCO1_Pos) | (5UL << RCC_CFGR_MCO1PRE_Pos);
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    pin_af(GPIOA, 8, 0);

    // The 1 ms SysTick was set up for 168 MHz.
    SYST_RVR = SYSCLK_HZ / 1000 - 1;
    SYST_CVR = 0;
    return true;
}

void eth_hw_init_pins(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_GPIOGEN;
    RCC->APB2ENR |= APB2ENR_SYSCFG;
    (void)RCC->APB2ENR;

    // RMII must be selected while the MAC is held in reset.
    RCC->AHB1RSTR |= AHB1RSTR_ETHMAC;
    SYSCFG_PMC_ADDR |= PMC_RMII_SEL;
    RCC->AHB1RSTR &= ~AHB1RSTR_ETHMAC;

    pin_af(GPIOA, 1, 11);
    pin_af(GPIOA, 2, 11);
    pin_af(GPIOA, 7, 11);
    pin_af(GPIOC, 1, 11);
    pin_af(GPIOC, 4, 11);
    pin_af(GPIOC, 5, 11);
    pin_af(GPIOG, 11, 11);
    pin_af(GPIOG, 13, 11);
    pin_af(GPIOG, 14, 11);

    // PHY reset: low for ~1 ms, then high and let it come up.
    GPIOG->BSRR = 1UL << (8 + 16);
    GPIOG->MODER = (GPIOG->MODER & ~(3UL << 16)) | (1UL << 16);
    spin(150000);
    GPIOG->BSRR = 1UL << 8;
    spin(1500000);
}

static bool mdio_read(uint8_t phy, uint8_t reg, uint16_t *val)
{
    ETH_MACMIIAR = ((uint32_t)phy << 11) | ((uint32_t)reg << 6) | MIIAR_CR_DIV102 | MIIAR_MB;
    for (uint32_t n = 0; n < 100000; n++) {
        if (!(ETH_MACMIIAR & MIIAR_MB)) {
            *val = (uint16_t)ETH_MACMIIDR;
            return true;
        }
    }
    return false;
}

int eth_hw_probe(uint32_t *phy_id)
{
    *phy_id = 0;
    RCC->AHB1ENR |= AHB1ENR_ETHMAC;
    (void)RCC->AHB1ENR;

    // A MAC software reset only completes with the 50 MHz REF_CLK present.
    ETH_DMABMR |= DMABMR_SR;
    uint32_t n = 0;
    while ((ETH_DMABMR & DMABMR_SR) && ++n < 2000000);
    if (ETH_DMABMR & DMABMR_SR)
        return ETH_HW_NO_REFCLK;

    for (uint8_t a = 0; a < 32; a++) {
        uint16_t id1, id2;
        if (mdio_read(a, 2, &id1) && mdio_read(a, 3, &id2) && id1 != 0xFFFF && id1 != 0) {
            *phy_id = ((uint32_t)id1 << 16) | id2;
            return a;
        }
    }
    return ETH_HW_NO_PHY;
}

void eth_hw_uid(uint8_t out[12])
{
    const volatile uint8_t *uid = (const volatile uint8_t *)0x1FFF7A10UL;
    for (int i = 0; i < 12; i++)
        out[i] = uid[i];
}

#endif // PMC_LINK_ETHERNET
