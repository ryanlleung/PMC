/* Simulator stand-in: the main loop pass is paced by sim_run(). */
#ifndef _SIM_DELAYS_H_
#define _SIM_DELAYS_H_
static inline void Delay_ms(unsigned ms) { (void)ms; }
#endif
