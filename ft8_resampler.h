#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FT8_AUDIO_RATE 12000U
#define FT8_RESAMPLER_TAPS 513U

struct ft8_resampler {
    uint32_t factor;
    uint32_t phase;
    uint32_t write_index;
    uint32_t oscillator_samples;
    double oscillator_cos;
    double oscillator_sin;
    double oscillator_step_cos;
    double oscillator_step_sin;
    float coefficients[FT8_RESAMPLER_TAPS];
    float history[FT8_RESAMPLER_TAPS];
};

bool ft8_resampler_init(struct ft8_resampler *state, uint32_t input_rate);
void ft8_resampler_reset(struct ft8_resampler *state);

/* Shift the IQ passband by +1500 Hz and emit 12 ksample/s real audio. */
bool ft8_resampler_process(struct ft8_resampler *state,
                           float input_i,
                           float input_q,
                           float *output);
