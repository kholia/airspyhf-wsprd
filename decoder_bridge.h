#pragma once

#include <stdint.h>

struct decoder_options {
    uint32_t freq;
    char rcall[13];
    char rloc[7];
    char date[7];
    char uttime[5];
    uint32_t quickmode;
    uint32_t usehashtable;
    uint32_t npasses;
    uint32_t subtraction;
};

struct decoder_results {
    double freq;
    float sync;
    float snr;
    float dt;
    float drift;
    int32_t jitter;
    char message[23];
    char call[13];
    char loc[7];
    char pwr[3];
    uint32_t cycles;
};

/* Run the synced wsprd core on one 375-sample/s complex WSPR frame. */
int decode_wspr_frame(const float *i_samples,
                      const float *q_samples,
                      uint32_t sample_count,
                      const struct decoder_options *options,
                      struct decoder_results *results,
                      uint32_t result_capacity,
                      int32_t *result_count);
