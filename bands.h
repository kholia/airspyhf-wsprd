#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WSPR_HOP_DWELL_SECONDS 600U
#define FT8_HOP_DWELL_SECONDS 120U
#define WSPR_MAX_HOP_BANDS 16U

struct wspr_hop_band {
    char name[8];
    uint32_t frequency_hz;
    int database_band;
};

struct wspr_hop_plan {
    struct wspr_hop_band bands[WSPR_MAX_HOP_BANDS];
    size_t count;
};

/* Resolve an Airspy HF+-tunable WSPR band name to its USB dial frequency. */
bool wspr_band_frequency(const char *name, uint32_t *frequency_hz);
const char *wspr_supported_bands(void);
bool ft8_band_frequency(const char *name, uint32_t *frequency_hz);
const char *ft8_supported_bands(void);

/* Parse a comma-separated list and choose a band in mode-specific UTC slots. */
bool wspr_hop_plan_parse(struct wspr_hop_plan *plan, const char *list);
bool ft8_hop_plan_parse(struct wspr_hop_plan *plan, const char *list);
const struct wspr_hop_band *wspr_hop_band_at(const struct wspr_hop_plan *plan,
                                             int64_t utc_seconds);
const struct wspr_hop_band *wspr_hop_band_at_dwell(
    const struct wspr_hop_plan *plan,
    int64_t utc_seconds,
    uint32_t dwell_seconds);

/* Use five ranked hops and reserve every sixth hop for discovery. */
const struct wspr_hop_band *wspr_adaptive_hop_band_at(
    const struct wspr_hop_plan *ranked,
    const struct wspr_hop_plan *candidates,
    int64_t utc_seconds,
    bool *exploration_slot);
const struct wspr_hop_band *wspr_adaptive_hop_band_at_dwell(
    const struct wspr_hop_plan *ranked,
    const struct wspr_hop_plan *candidates,
    int64_t utc_seconds,
    uint32_t dwell_seconds,
    bool *exploration_slot);
