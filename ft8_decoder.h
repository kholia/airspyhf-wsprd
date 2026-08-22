#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "common/monitor.h"

#define FT8_MAX_RESULTS 50U

struct ft8_decode_result {
    float snr;
    float dt;
    float audio_frequency_hz;
    char transmitter_call[13];
    char transmitter_grid[7];
    char message[FTX_MAX_MESSAGE_LENGTH];
};

struct ft8_decoder {
    monitor_t monitor;
    bool initialized;
};

bool ft8_decoder_init(struct ft8_decoder *decoder);
void ft8_decoder_free(struct ft8_decoder *decoder);
int ft8_decode_frame(struct ft8_decoder *decoder,
                     const float *samples,
                     size_t sample_count,
                     struct ft8_decode_result *results,
                     size_t result_capacity,
                     size_t *result_count);
