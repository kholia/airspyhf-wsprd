#pragma once

#include <stdbool.h>
#include <pthread.h>

#include "bands.h"

struct wspr_band_advisor {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct wspr_hop_plan candidates;
    struct wspr_hop_plan pending;
    char grid[7];
    bool started;
    bool stopping;
    bool update_ready;
    bool use_psk_reporter;
};

bool wspr_grid_center(const char *grid, double *longitude, double *latitude);
bool wspr_advisor_parse_response(const struct wspr_hop_plan *candidates,
                                 const char *response,
                                 struct wspr_hop_plan *ranked);
bool ft8_advisor_parse_response(const struct wspr_hop_plan *candidates,
                                const char *response,
                                struct wspr_hop_plan *recommended);
bool ft8_monitor_parse_response(const struct wspr_hop_plan *candidates,
                                const char *response,
                                struct wspr_hop_plan *ranked);
bool ft8_advisor_format_url(char *url,
                            size_t capacity,
                            const char *grid,
                            const struct wspr_hop_plan *candidates);
bool ft8_monitor_format_url(char *url, size_t capacity);
int wspr_band_advisor_start(struct wspr_band_advisor *advisor,
                            const char *grid,
                            const struct wspr_hop_plan *candidates);
int ft8_band_advisor_start(struct wspr_band_advisor *advisor,
                           const char *grid,
                           const struct wspr_hop_plan *candidates);
bool wspr_band_advisor_take(struct wspr_band_advisor *advisor,
                            struct wspr_hop_plan *plan);
bool wspr_band_advisor_wait(struct wspr_band_advisor *advisor,
                            struct wspr_hop_plan *plan,
                            unsigned int timeout_seconds);
void wspr_band_advisor_stop(struct wspr_band_advisor *advisor);
