/* Simulator stand-in for the mikroSDK/CMSIS STM32F407 header: the
 * registers the firmware touches are plain structs in RAM, read and written
 * by the bench model in sim_hw.c. Only what InitialTesting uses. */
#ifndef _SIM_MCU_H_
#define _SIM_MCU_H_

#include <stdint.h>

typedef struct { volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR, AFR[2]; } GPIO_TypeDef;
typedef struct {
    volatile uint32_t CR, PLLCFGR, CFGR, CIR, AHB1RSTR, AHB2RSTR, AHB3RSTR, APB1RSTR, APB2RSTR;
    volatile uint32_t AHB1ENR, AHB2ENR, AHB3ENR, APB1ENR, APB2ENR, BDCR, CSR;
} RCC_TypeDef;
typedef struct { volatile uint32_t CR1, CR2, SMCR, DIER, SR, EGR, CCMR1, CCMR2, CCER, CNT, PSC, ARR; } TIM_TypeDef;
typedef struct { volatile uint32_t SR, CR1, CR2, SMPR1, SMPR2, JOFR[4], HTR, LTR, SQR1, SQR2, SQR3, JSQR, JDR[4], DR; } ADC_TypeDef;
typedef struct { volatile uint32_t CSR, CCR, CDR; } ADC_Common_TypeDef;
typedef struct { volatile uint32_t TR, DR, CR, ISR, PRER, WUTR, CALIBR, ALRMAR, ALRMBR, WPR; } RTC_TypeDef;
typedef struct { volatile uint32_t CR, CSR; } PWR_TypeDef;
typedef struct { volatile uint32_t KR, PR, RLR, SR; } IWDG_TypeDef;

extern GPIO_TypeDef sim_gpio[7];
extern RCC_TypeDef sim_rcc;
extern TIM_TypeDef sim_tim7;
extern ADC_TypeDef sim_adc1;
extern ADC_Common_TypeDef sim_adc;
extern RTC_TypeDef sim_rtc;
extern PWR_TypeDef sim_pwr;
extern IWDG_TypeDef sim_iwdg;

#define GPIOA (&sim_gpio[0])
#define GPIOB (&sim_gpio[1])
#define GPIOC (&sim_gpio[2])
#define GPIOD (&sim_gpio[3])
#define GPIOE (&sim_gpio[4])
#define GPIOF (&sim_gpio[5])
#define GPIOG (&sim_gpio[6])
#define RCC   (&sim_rcc)
#define TIM7  (&sim_tim7)
// Each use of ADC1 finishes a conversion of the channel in SQR3 (sim_main.c).
ADC_TypeDef *sim_adc1_access(void);
#define ADC1  (sim_adc1_access())
#define ADC   (&sim_adc)
#define RTC   (&sim_rtc)
#define PWR   (&sim_pwr)
#define IWDG  (&sim_iwdg)

#define TIM7_IRQn 55

#define RCC_AHB1ENR_GPIOAEN   (1UL << 0)
#define RCC_AHB1ENR_GPIODEN   (1UL << 3)
#define RCC_AHB1ENR_GPIOGEN   (1UL << 6)
#define RCC_APB1ENR_TIM7EN    (1UL << 5)
#define RCC_APB1ENR_PWREN     (1UL << 28)
#define RCC_APB2ENR_ADC1EN    (1UL << 8)
#define RCC_BDCR_LSEON        (1UL << 0)
#define RCC_BDCR_LSERDY       (1UL << 1)
#define RCC_BDCR_RTCSEL       (3UL << 8)
#define RCC_BDCR_RTCEN        (1UL << 15)
#define RCC_BDCR_BDRST        (1UL << 16)
#define RCC_CSR_RMVF          (1UL << 24)
#define RCC_CSR_BORRSTF       (1UL << 25)
#define RCC_CSR_PINRSTF       (1UL << 26)
#define RCC_CSR_PORRSTF       (1UL << 27)
#define RCC_CSR_SFTRSTF       (1UL << 28)
#define RCC_CSR_IWDGRSTF      (1UL << 29)
#define RCC_CSR_WWDGRSTF      (1UL << 30)
#define RCC_CSR_LPWRRSTF      (1UL << 31)
#define RCC_CFGR_SWS          (3UL << 2)
#define RCC_CFGR_SWS_HSI      (0UL << 2)
#define RCC_CFGR_SWS_HSE      (1UL << 2)
#define RCC_CFGR_HPRE_Pos     4
#define RCC_CFGR_HPRE_Msk     (0xFUL << 4)
#define RCC_CFGR_PPRE1_Pos    10
#define RCC_CFGR_PPRE1_Msk    (7UL << 10)
#define RCC_PLLCFGR_PLLM_Pos  0
#define RCC_PLLCFGR_PLLM_Msk  (0x3FUL << 0)
#define RCC_PLLCFGR_PLLN_Pos  6
#define RCC_PLLCFGR_PLLN_Msk  (0x1FFUL << 6)
#define RCC_PLLCFGR_PLLP_Pos  16
#define RCC_PLLCFGR_PLLP_Msk  (3UL << 16)
#define RCC_PLLCFGR_PLLSRC    (1UL << 22)

#define TIM_CR1_CEN   (1UL << 0)
#define TIM_CR1_URS   (1UL << 2)
#define TIM_CR1_ARPE  (1UL << 7)
#define TIM_DIER_UIE  (1UL << 0)
#define TIM_EGR_UG    (1UL << 0)

#define ADC_SR_EOC            (1UL << 1)
#define ADC_CR2_ADON          (1UL << 0)
#define ADC_CR2_SWSTART       (1UL << 30)
#define ADC_CCR_ADCPRE        (3UL << 16)
#define ADC_CCR_ADCPRE_0      (1UL << 16)
#define ADC_CCR_TSVREFE       (1UL << 23)
#define ADC_SMPR1_SMP17_Pos   21
#define ADC_SMPR1_SMP17       (7UL << 21)
#define ADC_SMPR2_SMP3_Pos    9
#define ADC_SMPR2_SMP3        (7UL << 9)

#define RTC_ISR_INITS (1UL << 4)
#define RTC_ISR_RSF   (1UL << 5)
#define RTC_ISR_INITF (1UL << 6)
#define RTC_ISR_INIT  (1UL << 7)
#define RTC_CR_FMT    (1UL << 6)
#define PWR_CR_DBP    (1UL << 8)

#endif
