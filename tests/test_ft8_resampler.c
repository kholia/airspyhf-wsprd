#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "ft8_resampler.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double measure_audio(uint32_t input_rate, double audio_frequency_hz)
{
    struct ft8_resampler state;
    const uint32_t output_samples = FT8_AUDIO_RATE * 2U;
    const uint64_t input_samples =
        (uint64_t)output_samples * input_rate / FT8_AUDIO_RATE;
    double power = 0.0;
    uint32_t measured = 0U;

    assert(ft8_resampler_init(&state, input_rate));
    for (uint64_t index = 0U; index < input_samples; index++) {
        double iq_frequency_hz = audio_frequency_hz - 1500.0;
        double phase = 2.0 * M_PI * iq_frequency_hz * index / input_rate;
        float output;

        if (ft8_resampler_process(&state, (float)cos(phase), (float)sin(phase),
                                  &output)) {
            if (measured > 600U) {
                power += output * output;
            }
            measured++;
        }
    }
    assert(measured == output_samples);
    return sqrt(power / (measured - 601U));
}

int main(void)
{
    struct ft8_resampler state;
    double wanted = measure_audio(192000U, 1234.0);
    double high_edge = measure_audio(192000U, 3000.0);
    double alias = measure_audio(192000U, 9000.0);

    assert(!ft8_resampler_init(&state, 256000U));
    assert(ft8_resampler_init(&state, 192000U));
    assert(state.factor == 16U);
    assert(wanted > 0.65);
    assert(high_edge > 0.64);
    assert(alias < wanted * 0.001);
    printf("ft8 resampler: 1234 Hz %.6f, 3000 Hz %.6f, 9000 Hz alias %.8f\n",
           wanted, high_edge, alias);
    return 0;
}
