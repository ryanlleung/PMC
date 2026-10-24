/* Simulator stand-in: mikroBUS socket 1 pin names used by stepper3.c. */
#ifndef _SIM_BOARD_H_
#define _SIM_BOARD_H_
typedef int pin_name_t;
#define MIKROBUS_1_AN  0x04   /* PA4 */
#define MIKROBUS_1_RST 0x22   /* PC2 */
#define MIKROBUS_1_CS  0x1C   /* PB12 */
#define MIKROBUS_1_PWM 0x3C   /* PD12 */
#endif
