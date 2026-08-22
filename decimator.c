#include <math.h>
#include <stddef.h>
#include <string.h>

#include "decimator.h"

/* Compensation filter for the two-stage, differential-delay-two CIC. */
static const float compensation_fir[WSPR_FIR_TAPS] = {
    -0.0027772683f, -0.0005058826f,  0.0049745750f, -0.0034059318f,
    -0.0077557814f,  0.0139375423f,  0.0039896935f, -0.0299394142f,
     0.0162250643f,  0.0405130860f, -0.0580746013f, -0.0272104968f,
     0.1183705475f, -0.0306029022f, -0.2011241667f,  0.1615898423f,
     0.5000000000f,
     0.1615898423f, -0.2011241667f, -0.0306029022f,  0.1183705475f,
    -0.0272104968f, -0.0580746013f,  0.0405130860f,  0.0162250643f,
    -0.0299394142f,  0.0039896935f,  0.0139375423f, -0.0077557814f,
    -0.0034059318f,  0.0049745750f, -0.0005058826f, -0.0027772683f
};

bool wspr_decimator_init(struct wspr_decimator *state, uint32_t input_rate)
{
    if (state == NULL || input_rate == 0 || input_rate % WSPR_OUTPUT_RATE != 0) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->factor = input_rate / WSPR_OUTPUT_RATE;
    return true;
}

void wspr_decimator_reset(struct wspr_decimator *state)
{
    uint32_t factor;

    if (state == NULL) {
        return;
    }

    factor = state->factor;
    memset(state, 0, sizeof(*state));
    state->factor = factor;
}

bool wspr_decimator_process(struct wspr_decimator *state,
                            float input_i,
                            float input_q,
                            float *output_i,
                            float *output_q)
{
    double i_comb_1;
    double q_comb_1;
    double i_comb_2;
    double q_comb_2;
    double i_sum = 0.0;
    double q_sum = 0.0;
    double scale;
    size_t index;

    if (state == NULL || state->factor == 0 || output_i == NULL || output_q == NULL) {
        return false;
    }

    state->i_integrator_1 += input_i;
    state->q_integrator_1 += input_q;
    state->i_integrator_2 += state->i_integrator_1;
    state->q_integrator_2 += state->q_integrator_1;

    state->phase++;
    if (state->phase < state->factor) {
        return false;
    }
    state->phase = 0;

    i_comb_1 = state->i_integrator_2 - state->i_comb_1_z2;
    q_comb_1 = state->q_integrator_2 - state->q_comb_1_z2;
    state->i_comb_1_z2 = state->i_comb_1_z1;
    state->q_comb_1_z2 = state->q_comb_1_z1;
    state->i_comb_1_z1 = state->i_integrator_2;
    state->q_comb_1_z1 = state->q_integrator_2;

    i_comb_2 = i_comb_1 - state->i_comb_2_z2;
    q_comb_2 = q_comb_1 - state->q_comb_2_z2;
    state->i_comb_2_z2 = state->i_comb_2_z1;
    state->q_comb_2_z2 = state->q_comb_2_z1;
    state->i_comb_2_z1 = i_comb_1;
    state->q_comb_2_z1 = q_comb_1;

    for (index = 0; index + 1 < WSPR_FIR_TAPS; index++) {
        state->i_fir[index] = state->i_fir[index + 1];
        state->q_fir[index] = state->q_fir[index + 1];
    }
    state->i_fir[WSPR_FIR_TAPS - 1] = (float)i_comb_2;
    state->q_fir[WSPR_FIR_TAPS - 1] = (float)q_comb_2;

    for (index = 0; index < WSPR_FIR_TAPS; index++) {
        i_sum += state->i_fir[index] * compensation_fir[index];
        q_sum += state->q_fir[index] * compensation_fir[index];
    }

    /* CIC DC gain is (factor * differential delay)^2. */
    scale = 4.0 * state->factor * state->factor;
    *output_i = (float)(i_sum / scale);
    *output_q = (float)(q_sum / scale);
    return true;
}
