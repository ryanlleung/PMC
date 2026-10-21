#ifndef _MODEL_H_
#define _MODEL_H_

#include <stdbool.h>
#include <stdint.h>

#define MODEL_DT 0.01   // simulator step, s

typedef struct {
    double t;                 // s
    // flow line
    double P, T, mL;          // line pressure (mbar), pot temperature, liquid (g)
    bool he, pump;            // helium supply, rotary pump
    // stepper
    int32_t pos, target;      // step count
    bool moving;
    int dir;
    double v, acc, vrun, vstart;
    double run, start, accel; // MOT RATE, START, ACCEL
    const char *last_stop;
    bool coupled;             // drive turns the needle (false: slipping)
    int32_t pos_max;
    // the needle and the closed switch
    int32_t sim_pos;          // mechanism position, steps (seat 30 past the switch)
    bool lim_raw, lim_active, clear_on;
    double clear_since;
    // reading
    double next_meas;
    double pv;                // mbar, through the bench cal
} model_t;

extern model_t M;

void model_init(void);
bool model_tick(void);              // true after a 1 s pressure read
bool model_start_move(int32_t target);
void model_stop(void);
double model_random(void);

#endif // _MODEL_H_
