#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "decimator.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double measure_tone(uint32_t input_rate, double tone_hz)
{
    struct wspr_decimator state;
    const uint32_t output_samples = WSPR_OUTPUT_RATE * 3;
    const uint64_t input_samples = (uint64_t)output_samples * input_rate / WSPR_OUTPUT_RATE;
    double power = 0.0;
    uint32_t measured = 0;

    assert(wspr_decimator_init(&state, input_rate));
    for (uint64_t i = 0; i < input_samples; i++) {
        double phase = 2.0 * M_PI * tone_hz * i / input_rate;
        float output_i;
        float output_q;

        if (wspr_decimator_process(&state, (float)cos(phase), (float)sin(phase),
                                   &output_i, &output_q)) {
            if (measured >= WSPR_FIR_TAPS + 8) {
                power += output_i * output_i + output_q * output_q;
            }
            measured++;
        }
    }

    assert(measured == output_samples);
    return sqrt(power / (measured - WSPR_FIR_TAPS - 8));
}

int main(void)
{
    struct wspr_decimator state;
    double wanted;
    double alias;

    assert(!wspr_decimator_init(&state, 256000));
    assert(wspr_decimator_init(&state, 192000));
    assert(state.factor == 512);
    assert(wspr_decimator_init(&state, 768000));
    assert(state.factor == 2048);

    /* A blocker one output sample-rate above a wanted tone aliases onto it. */
    wanted = measure_tone(192000, 75.0);
    alias = measure_tone(192000, WSPR_OUTPUT_RATE + 75.0);
    assert(wanted > 0.25);
    assert(alias < wanted * 0.04);

    printf("decimator: wanted amplitude %.6f, first-alias amplitude %.6f\n", wanted, alias);
    return 0;
}
