#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bands.h"

int main(void)
{
    struct wspr_hop_plan plan;
    struct wspr_hop_plan ranked;
    struct wspr_hop_plan candidates;
    static const struct {
        const char *name;
        uint32_t frequency;
    } expected[] = {
        {"LF", 136000}, {"2200m", 136000},
        {"MF", 474200}, {"630M", 474200},
        {"160m", 1836600}, {"80m", 3568600}, {"60m", 5287200},
        {"40m", 7038600}, {"30m", 10138700}, {"20M", 14095600},
        {"17m", 18104600}, {"15m", 21094600}, {"12m", 24924600},
        {"10m", 28124600}, {"4m", 70091000}, {"2m", 144489000},
        {"1.25m", 222280000}, {"1m25", 222280000}
    };

    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]); index++) {
        uint32_t actual = 0;
        assert(wspr_band_frequency(expected[index].name, &actual));
        assert(actual == expected[index].frequency);
    }

    uint32_t unused = 0;
    assert(!wspr_band_frequency("6m", &unused));
    assert(!wspr_band_frequency("not-a-band", &unused));
    assert(ft8_band_frequency("20M", &unused));
    assert(unused == 14074000U);
    assert(ft8_band_frequency("40m", &unused));
    assert(unused == 7074000U);
    assert(!ft8_band_frequency("2200m", &unused));
    assert(wspr_hop_plan_parse(&plan, "80m, 40M,20m,10m"));
    assert(plan.count == 4U);
    assert(strcmp(plan.bands[0].name, "80m") == 0);
    assert(strcmp(plan.bands[1].name, "40m") == 0);
    assert(wspr_hop_band_at(&plan, 0)->frequency_hz == 3568600U);
    assert(wspr_hop_band_at(&plan, 599)->frequency_hz == 3568600U);
    assert(wspr_hop_band_at(&plan, 600)->frequency_hz == 7038600U);
    assert(wspr_hop_band_at(&plan, 2400)->frequency_hz == 3568600U);
    assert(wspr_hop_band_at(&plan, -1) == NULL);
    assert(!wspr_hop_plan_parse(&plan, "20m"));
    assert(!wspr_hop_plan_parse(&plan, "20m,20M"));
    assert(!wspr_hop_plan_parse(&plan, "20m,,40m"));
    assert(!wspr_hop_plan_parse(&plan, "20m,6m"));

    assert(ft8_hop_plan_parse(&plan, "80m,40M,20m,10m"));
    assert(plan.count == 4U);
    assert(plan.bands[0].frequency_hz == 3573000U);
    assert(plan.bands[1].frequency_hz == 7074000U);
    assert(plan.bands[2].frequency_hz == 14074000U);
    assert(plan.bands[3].frequency_hz == 28074000U);
    assert(strcmp(wspr_hop_band_at_dwell(&plan, 0,
                                         FT8_HOP_DWELL_SECONDS)->name,
                  "80m") == 0);
    assert(strcmp(wspr_hop_band_at_dwell(&plan, 120,
                                         FT8_HOP_DWELL_SECONDS)->name,
                  "40m") == 0);
    assert(wspr_hop_band_at_dwell(&plan, 120, 0U) == NULL);
    assert(!ft8_hop_plan_parse(&plan, "20m"));
    assert(!ft8_hop_plan_parse(&plan, "20m,6m"));

    assert(wspr_hop_plan_parse(&ranked, "20m,40m,10m"));
    assert(wspr_hop_plan_parse(&candidates,
                               "80m,40m,30m,20m,17m,15m,12m,10m"));
    bool exploring = true;
    assert(strcmp(wspr_adaptive_hop_band_at(&ranked, &candidates, 0,
                                             &exploring)->name, "20m") == 0);
    assert(!exploring);
    assert(strcmp(wspr_adaptive_hop_band_at(&ranked, &candidates, 3000,
                                             &exploring)->name, "80m") == 0);
    assert(exploring);
    assert(strcmp(wspr_adaptive_hop_band_at(&ranked, &candidates, 3600,
                                             &exploring)->name, "10m") == 0);
    assert(!exploring);
    assert(strcmp(wspr_adaptive_hop_band_at(&ranked, &candidates, 6600,
                                             &exploring)->name, "30m") == 0);
    assert(exploring);
    assert(strcmp(wspr_adaptive_hop_band_at_dwell(
                      &ranked, &candidates, 600, FT8_HOP_DWELL_SECONDS,
                      &exploring)->name, "80m") == 0);
    assert(exploring);
    assert(strcmp(wspr_adaptive_hop_band_at_dwell(
                      &ranked, &candidates, 720, FT8_HOP_DWELL_SECONDS,
                      &exploring)->name, "10m") == 0);
    assert(!exploring);
    assert(strcmp(wspr_adaptive_hop_band_at_dwell(
                      &ranked, &candidates, 1320, FT8_HOP_DWELL_SECONDS,
                      &exploring)->name, "30m") == 0);
    assert(exploring);
    assert(wspr_adaptive_hop_band_at(NULL, &candidates, 0, &exploring) == NULL);
    assert(!exploring);
    assert(strcmp(wspr_adaptive_hop_band_at(&ranked, &ranked, 3000,
                                             &exploring)->name, "10m") == 0);
    assert(!exploring);
    assert(strcmp(wspr_adaptive_hop_band_at(&ranked, &ranked, 3600,
                                             &exploring)->name, "20m") == 0);
    printf("bands: verified %zu WSPR band aliases\n",
           sizeof(expected) / sizeof(expected[0]));
    return 0;
}
