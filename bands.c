#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bands.h"

struct band_frequency {
    const char *alias;
    const char *canonical_name;
    uint32_t frequency_hz;
    int database_band;
};

static const struct band_frequency band_frequencies[] = {
    {"lf",     "2200m",   136000,  -1},
    {"2200m",  "2200m",   136000,  -1},
    {"mf",     "630m",    474200,   0},
    {"630m",   "630m",    474200,   0},
    {"160m",   "160m",   1836600,   1},
    {"80m",    "80m",    3568600,   3},
    {"60m",    "60m",    5287200,   5},
    {"40m",    "40m",    7038600,   7},
    {"30m",    "30m",   10138700,  10},
    {"20m",    "20m",   14095600,  14},
    {"17m",    "17m",   18104600,  18},
    {"15m",    "15m",   21094600,  21},
    {"12m",    "12m",   24924600,  24},
    {"10m",    "10m",   28124600,  28},
    {"4m",     "4m",    70091000,  70},
    {"2m",     "2m",   144489000, 144},
    {"1.25m",  "1.25m", 222280000, 222},
    {"1m25",   "1.25m", 222280000, 222}
};

static const struct band_frequency ft8_band_frequencies[] = {
    {"160m", "160m",  1840000,  1},
    {"80m",  "80m",   3573000,  3},
    {"60m",  "60m",   5357000,  5},
    {"40m",  "40m",   7074000,  7},
    {"30m",  "30m",  10136000, 10},
    {"20m",  "20m",  14074000, 14},
    {"17m",  "17m",  18100000, 18},
    {"15m",  "15m",  21074000, 21},
    {"12m",  "12m",  24915000, 24},
    {"10m",  "10m",  28074000, 28}
};

static const struct band_frequency *find_band(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (size_t index = 0;
         index < sizeof(band_frequencies) / sizeof(band_frequencies[0]);
         index++) {
        if (strcasecmp(name, band_frequencies[index].alias) == 0) {
            return &band_frequencies[index];
        }
    }
    return NULL;
}

static const struct band_frequency *find_ft8_band(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t index = 0U;
         index < sizeof(ft8_band_frequencies) / sizeof(ft8_band_frequencies[0]);
         index++) {
        if (strcasecmp(name, ft8_band_frequencies[index].alias) == 0) {
            return &ft8_band_frequencies[index];
        }
    }
    return NULL;
}

bool wspr_band_frequency(const char *name, uint32_t *frequency_hz)
{
    const struct band_frequency *band = find_band(name);

    if (band == NULL || frequency_hz == NULL) {
        return false;
    }
    *frequency_hz = band->frequency_hz;
    return true;
}

const char *wspr_supported_bands(void)
{
    return "2200m 630m 160m 80m 60m 40m 30m 20m 17m 15m 12m 10m 4m 2m 1.25m";
}

bool ft8_band_frequency(const char *name, uint32_t *frequency_hz)
{
    const struct band_frequency *band = find_ft8_band(name);

    if (band == NULL || frequency_hz == NULL) {
        return false;
    }
    *frequency_hz = band->frequency_hz;
    return true;
}

const char *ft8_supported_bands(void)
{
    return "160m 80m 60m 40m 30m 20m 17m 15m 12m 10m";
}

static bool hop_plan_parse(struct wspr_hop_plan *plan,
                           const char *list,
                           bool ft8)
{
    char buffer[256];
    char *field;

    if (plan == NULL || list == NULL || *list == '\0' ||
        snprintf(buffer, sizeof(buffer), "%s", list) >= (int)sizeof(buffer)) {
        return false;
    }
    memset(plan, 0, sizeof(*plan));
    field = buffer;

    while (field != NULL) {
        char *comma = strchr(field, ',');
        char *end;
        const struct band_frequency *band;

        if (comma != NULL) {
            *comma = '\0';
        }
        while (isspace((unsigned char)*field)) {
            field++;
        }
        end = field + strlen(field);
        while (end > field && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        band = ft8 ? find_ft8_band(field) : find_band(field);
        if (band == NULL || plan->count >= WSPR_MAX_HOP_BANDS) {
            memset(plan, 0, sizeof(*plan));
            return false;
        }
        for (size_t index = 0; index < plan->count; index++) {
            if (strcmp(plan->bands[index].name, band->canonical_name) == 0) {
                memset(plan, 0, sizeof(*plan));
                return false;
            }
        }
        if (snprintf(plan->bands[plan->count].name,
                     sizeof(plan->bands[plan->count].name), "%s",
                     band->canonical_name) >=
            (int)sizeof(plan->bands[plan->count].name)) {
            memset(plan, 0, sizeof(*plan));
            return false;
        }
        plan->bands[plan->count].frequency_hz = band->frequency_hz;
        plan->bands[plan->count].database_band = band->database_band;
        plan->count++;

        field = comma == NULL ? NULL : comma + 1;
        if (field != NULL && *field == '\0') {
            memset(plan, 0, sizeof(*plan));
            return false;
        }
    }
    return plan->count >= 2U;
}

bool wspr_hop_plan_parse(struct wspr_hop_plan *plan, const char *list)
{
    return hop_plan_parse(plan, list, false);
}

bool ft8_hop_plan_parse(struct wspr_hop_plan *plan, const char *list)
{
    return hop_plan_parse(plan, list, true);
}

const struct wspr_hop_band *wspr_hop_band_at(const struct wspr_hop_plan *plan,
                                             int64_t utc_seconds)
{
    return wspr_hop_band_at_dwell(plan, utc_seconds,
                                  WSPR_HOP_DWELL_SECONDS);
}

const struct wspr_hop_band *wspr_hop_band_at_dwell(
    const struct wspr_hop_plan *plan,
    int64_t utc_seconds,
    uint32_t dwell_seconds)
{
    uint64_t slot;

    if (plan == NULL || plan->count == 0U || utc_seconds < 0 ||
        dwell_seconds == 0U) {
        return NULL;
    }
    slot = (uint64_t)utc_seconds / dwell_seconds;
    return &plan->bands[slot % plan->count];
}

static bool plan_contains_database_band(const struct wspr_hop_plan *plan,
                                        int database_band)
{
    for (size_t index = 0; index < plan->count; index++) {
        if (plan->bands[index].database_band == database_band) {
            return true;
        }
    }
    return false;
}

const struct wspr_hop_band *wspr_adaptive_hop_band_at(
    const struct wspr_hop_plan *ranked,
    const struct wspr_hop_plan *candidates,
    int64_t utc_seconds,
    bool *exploration_slot)
{
    return wspr_adaptive_hop_band_at_dwell(
        ranked, candidates, utc_seconds, WSPR_HOP_DWELL_SECONDS,
        exploration_slot);
}

const struct wspr_hop_band *wspr_adaptive_hop_band_at_dwell(
    const struct wspr_hop_plan *ranked,
    const struct wspr_hop_plan *candidates,
    int64_t utc_seconds,
    uint32_t dwell_seconds,
    bool *exploration_slot)
{
    uint64_t slot;
    size_t excluded_count = 0U;

    if (exploration_slot != NULL) {
        *exploration_slot = false;
    }
    if (ranked == NULL || ranked->count == 0U || candidates == NULL ||
        candidates->count == 0U || utc_seconds < 0 || dwell_seconds == 0U) {
        return NULL;
    }
    slot = (uint64_t)utc_seconds / dwell_seconds;
    for (size_t index = 0; index < candidates->count; index++) {
        if (!plan_contains_database_band(ranked,
                                         candidates->bands[index].database_band)) {
            excluded_count++;
        }
    }

    if (slot % 6U == 5U && excluded_count != 0U) {
        size_t wanted = (size_t)((slot / 6U) % excluded_count);

        for (size_t index = 0; index < candidates->count; index++) {
            if (plan_contains_database_band(ranked,
                                            candidates->bands[index].database_band)) {
                continue;
            }
            if (wanted == 0U) {
                if (exploration_slot != NULL) {
                    *exploration_slot = true;
                }
                return &candidates->bands[index];
            }
            wanted--;
        }
    }

    /* Removing one slot per hour keeps the ranked rotation continuous. */
    uint64_t ranked_slot = excluded_count == 0U ? slot : slot - slot / 6U;
    return &ranked->bands[ranked_slot % ranked->count];
}
