/* Simulator stand-in for the mikroSDK digital output driver. */
#ifndef _SIM_DRV_DIGITAL_OUT_H_
#define _SIM_DRV_DIGITAL_OUT_H_
#include "board.h"
typedef struct { pin_name_t pin; } digital_out_t;
static inline int digital_out_init(digital_out_t *o, pin_name_t p) { o->pin = p; return 0; }
static inline void digital_out_low(digital_out_t *o) { (void)o; }
#endif
