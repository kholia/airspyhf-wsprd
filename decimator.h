#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WSPR_OUTPUT_RATE 375U
#define WSPR_FIR_TAPS 33U

struct wspr_decimator {
    uint32_t factor;
    uint32_t phase;

    double i_integrator_1;
    double i_integrator_2;
    double q_integrator_1;
    double q_integrator_2;

    double i_comb_1_z1;
    double i_comb_1_z2;
    double i_comb_2_z1;
    double i_comb_2_z2;
    double q_comb_1_z1;
    double q_comb_1_z2;
    double q_comb_2_z1;
    double q_comb_2_z2;

    float i_fir[WSPR_FIR_TAPS];
    float q_fir[WSPR_FIR_TAPS];
};

/* Returns false when input_rate cannot be converted exactly to 375 samples/s. */
bool wspr_decimator_init(struct wspr_decimator *state, uint32_t input_rate);
void wspr_decimator_reset(struct wspr_decimator *state);

/* Returns true and writes one complex output sample whenever decimation completes. */
bool wspr_decimator_process(struct wspr_decimator *state,
                            float input_i,
                            float input_q,
                            float *output_i,
                            float *output_q);
