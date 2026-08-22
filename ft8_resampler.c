#include <math.h>
#include <stddef.h>
#include <string.h>

#include "ft8_resampler.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FT8_AUDIO_SHIFT_HZ 1500.0
#define FT8_FILTER_CUTOFF_HZ 4000.0
#define FT8_KAISER_BETA 8.6

static double bessel_i0(double value)
{
    double sum = 1.0;
    double term = 1.0;
    double quarter_square = value * value / 4.0;

    for (unsigned int order = 1U; order < 32U; order++) {
        term *= quarter_square / ((double)order * order);
        sum += term;
        if (term < sum * 1e-15) {
            break;
        }
    }
    return sum;
}

static void design_filter(struct ft8_resampler *state, uint32_t input_rate)
{
    const double center = (FT8_RESAMPLER_TAPS - 1U) / 2.0;
    const double normalized_cutoff = FT8_FILTER_CUTOFF_HZ / input_rate;
    const double window_scale = bessel_i0(FT8_KAISER_BETA);
    double coefficient_sum = 0.0;

    for (uint32_t index = 0U; index < FT8_RESAMPLER_TAPS; index++) {
        double offset = index - center;
        double ideal;
        double position = offset / center;
        double window = bessel_i0(FT8_KAISER_BETA *
                                  sqrt(fmax(0.0, 1.0 - position * position))) /
                        window_scale;

        if (offset == 0.0) {
            ideal = 2.0 * normalized_cutoff;
        } else {
            ideal = sin(2.0 * M_PI * normalized_cutoff * offset) /
                    (M_PI * offset);
        }
        state->coefficients[index] = (float)(ideal * window);
        coefficient_sum += state->coefficients[index];
    }
    for (uint32_t index = 0U; index < FT8_RESAMPLER_TAPS; index++) {
        state->coefficients[index] =
            (float)(state->coefficients[index] / coefficient_sum);
    }
}

bool ft8_resampler_init(struct ft8_resampler *state, uint32_t input_rate)
{
    double step;

    if (state == NULL || input_rate < FT8_AUDIO_RATE ||
        input_rate % FT8_AUDIO_RATE != 0U) {
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->factor = input_rate / FT8_AUDIO_RATE;
    state->oscillator_cos = 1.0;
    step = 2.0 * M_PI * FT8_AUDIO_SHIFT_HZ / input_rate;
    state->oscillator_step_cos = cos(step);
    state->oscillator_step_sin = sin(step);
    design_filter(state, input_rate);
    return true;
}

void ft8_resampler_reset(struct ft8_resampler *state)
{
    uint32_t factor;
    double step_cos;
    double step_sin;
    float coefficients[FT8_RESAMPLER_TAPS];

    if (state == NULL) {
        return;
    }
    factor = state->factor;
    step_cos = state->oscillator_step_cos;
    step_sin = state->oscillator_step_sin;
    memcpy(coefficients, state->coefficients, sizeof(coefficients));
    memset(state, 0, sizeof(*state));
    state->factor = factor;
    state->oscillator_cos = 1.0;
    state->oscillator_step_cos = step_cos;
    state->oscillator_step_sin = step_sin;
    memcpy(state->coefficients, coefficients, sizeof(coefficients));
}

bool ft8_resampler_process(struct ft8_resampler *state,
                           float input_i,
                           float input_q,
                           float *output)
{
    double next_cos;
    double next_sin;
    double sum = 0.0;

    if (state == NULL || state->factor == 0U || output == NULL) {
        return false;
    }

    state->history[state->write_index] =
        (float)(input_i * state->oscillator_cos -
                input_q * state->oscillator_sin);
    state->write_index = (state->write_index + 1U) % FT8_RESAMPLER_TAPS;

    next_cos = state->oscillator_cos * state->oscillator_step_cos -
               state->oscillator_sin * state->oscillator_step_sin;
    next_sin = state->oscillator_sin * state->oscillator_step_cos +
               state->oscillator_cos * state->oscillator_step_sin;
    state->oscillator_cos = next_cos;
    state->oscillator_sin = next_sin;
    state->oscillator_samples++;
    if (state->oscillator_samples == 4096U) {
        double magnitude = hypot(state->oscillator_cos, state->oscillator_sin);
        state->oscillator_cos /= magnitude;
        state->oscillator_sin /= magnitude;
        state->oscillator_samples = 0U;
    }

    state->phase++;
    if (state->phase < state->factor) {
        return false;
    }
    state->phase = 0U;

    for (uint32_t index = 0U; index < FT8_RESAMPLER_TAPS; index++) {
        uint32_t history_index =
            (state->write_index + index) % FT8_RESAMPLER_TAPS;
        sum += state->coefficients[index] * state->history[history_index];
    }
    *output = (float)sum;
    return true;
}
