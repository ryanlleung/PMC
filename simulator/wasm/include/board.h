/* Simulator stand-in: mikroBUS socket 2 pin names used by stepper3.c
 * (socket 1 to 0.14.1). */
#ifndef _SIM_BOARD_H_
#define _SIM_BOARD_H_
typedef int pin_name_t;
#define MIKROBUS_2_AN  0x10   /* PB0 */
#define MIKROBUS_2_RST 0x23   /* PC3 */
#define MIKROBUS_2_CS  0x0F   /* PA15 */
#define MIKROBUS_2_PWM 0x3D   /* PD13 */
#endif
