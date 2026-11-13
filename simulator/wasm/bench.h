/* The bench model (bench.c): flow line, needle, limit switch, Druck,
 * motor supply and controller temperature. */
#ifndef _BENCH_H_
#define _BENCH_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum { PLANT_BENCH, PLANT_PHYSICS } plant_t;
typedef enum { LIMFAULT_NONE, LIMFAULT_BROKEN, LIMFAULT_STUCK, LIMFAULT_EARLY } limfault_t;

typedef struct {
    // flow line
    int plant;                // PLANT_BENCH (fitted to the bench logs) or PLANT_PHYSICS
    double P, T, mL;          // line pressure (mbar), pot temperature (K), liquid (g): physics model
    double hx;                // heat exchanger temperature (K): bench model
    bool he, pump;            // helium supply, rotary pump
    // needle and motor
    int32_t needle_half;      // needle lift in half steps from the switch point
    int rotor;                // rotor phase, index into the half-step sequence
    uint8_t coils;            // coils energised now
    bool coupled;             // drive turns the needle (false: slipping)
    int32_t slipped, stalled; // steps lost to slipping / to the end stops
    // limit switch
    bool lim_raw;
    int limit_fault;          // limfault_t
    int32_t early_half;       // LIMFAULT_EARLY: trips this far above the seat
    // sensors
    bool druck, pm;           // Druck plugged in, INA228 answering
    double exc_v;             // Druck excitation from the Boost 10 (F-02 outside 9.5-10.5 V)
    bool vent;                // Druck open to room air (bench calibration)
    double room_mbar;
    bool baro;                // Pressure 21 fitted (0.14.0); it reads room_mbar + baro_err
    double baro_err;
    double supply_v;          // motor 12 V supply
    double die_c;             // INA228 die temperature (W-03 at 55 C)
} bench_t;

extern bench_t B;

void bench_init(void);
void bench_tick_10ms(void);
void bench_plant(int plant);            // plant_t; the bench model starts at its steady state for the pressure now
void bench_set_pressure(double mbar);   // the line pressure now (the bench model's delay too)
void bench_set_hx(double k);            // the heat exchanger now (the bench model: both its parts)
void bench_coils(uint8_t coils);
bool bench_limit_pin_high(void);
void bench_read(int32_t *raw_shunt, int32_t *raw_bus, int16_t *raw_temp);

#endif
