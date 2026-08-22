#pragma once

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "bands.h"

typedef size_t (*wspr_health_queue_depth)(void *context);

struct wspr_health_server {
    pthread_t thread;
    pthread_mutex_t mutex;
    int listen_fd;
    uint16_t port;
    bool started;
    bool stopping;
    bool clock_synchronized;
    bool decoder_busy;
    bool reporting_enabled;
    bool hopping_enabled;
    bool adaptive_hopping;
    bool exploration_slot;
    uint32_t dial_frequency_hz;
    uint32_t hop_interval_seconds;
    uint64_t frames_decoded;
    uint64_t spots_decoded;
    uint64_t decoder_errors;
    uint32_t last_decodes_ft8lib;
    uint32_t last_decodes_jt9;
    uint64_t total_decodes_ft8lib;
    uint64_t total_decodes_jt9;
    uint64_t frames_decoded_ft8lib;
    uint64_t frames_decoded_jt9;
    uint64_t airspy_overrun_samples;
    uint64_t dsp_queue_overrun_samples;
    time_t started_unix;
    time_t last_decode_unix;
    time_t last_spot_unix;
    char bind_address[16];
    char receiver_state[24];
    char mode[8];
    char callsign[13];
    char grid[7];
    char band[8];
    char selected_bands_json[256];
    wspr_health_queue_depth queue_depth;
    void *queue_context;
};

int wspr_health_server_start(struct wspr_health_server *server,
                             const char *bind_address,
                             uint16_t port,
                             const char *mode,
                             const char *callsign,
                             const char *grid,
                             bool reporting_enabled,
                             bool hopping_enabled,
                             bool adaptive_hopping,
                             wspr_health_queue_depth queue_depth,
                             void *queue_context);
void wspr_health_set_receiver_state(struct wspr_health_server *server,
                                    const char *state);
void wspr_health_set_clock(struct wspr_health_server *server, bool synchronized);
void wspr_health_set_tuning(struct wspr_health_server *server,
                            const char *band,
                            uint32_t dial_frequency_hz,
                            bool exploration_slot);
void wspr_health_set_selected_bands(struct wspr_health_server *server,
                                    const struct wspr_hop_plan *plan);
void wspr_health_set_decoder_busy(struct wspr_health_server *server, bool busy);
void wspr_health_record_decode(struct wspr_health_server *server,
                               bool success,
                               uint32_t spot_count,
                               uint64_t airspy_overrun_samples,
                               uint64_t dsp_queue_overrun_samples);
void wspr_health_record_ft8_decoders(struct wspr_health_server *server,
                                     bool ft8lib_success,
                                     uint32_t ft8lib_count,
                                     bool jt9_success,
                                     uint32_t jt9_count);
void wspr_health_server_stop(struct wspr_health_server *server);
