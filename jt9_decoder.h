#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "ft8_decoder.h"

#define JT9_DECODE_TIMEOUT_SECONDS 14U
#define JT9_WARMUP_TIMEOUT_SECONDS 60U

struct jt9_decoder {
    char *executable;
    char *runtime_directory;
    char callsign[13];
    char grid[7];
    char last_error[192];
    bool enabled;
};

struct jt9_decode_job {
    pid_t child;
    char *wave_path;
    char *output_path;
    struct timespec started_at;
    uint32_t timeout_seconds;
    bool active;
};

bool jt9_decoder_init(struct jt9_decoder *decoder,
                      const char *executable,
                      const char *callsign,
                      const char *grid);
void jt9_decoder_free(struct jt9_decoder *decoder);

int jt9_decoder_start(struct jt9_decoder *decoder,
                      const float *samples,
                      size_t sample_count,
                      time_t frame_start_unix,
                      struct jt9_decode_job *job);
int jt9_decoder_finish(struct jt9_decoder *decoder,
                       struct jt9_decode_job *job,
                       struct ft8_decode_result *results,
                       size_t result_capacity,
                       size_t *result_count);
bool jt9_decoder_warmup(struct jt9_decoder *decoder);

bool jt9_parse_decode_line(const char *line,
                           struct ft8_decode_result *result);
size_t jt9_merge_results(struct ft8_decode_result *primary,
                         size_t primary_count,
                         size_t capacity,
                         const struct ft8_decode_result *additional,
                         size_t additional_count);
