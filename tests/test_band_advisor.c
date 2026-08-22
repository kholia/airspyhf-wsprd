#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "band_advisor.h"

int main(void)
{
    struct wspr_band_advisor advisor = {0};
    struct wspr_hop_plan candidates;
    struct wspr_hop_plan ranked;
    struct wspr_hop_plan recommended;
    char url[512];
    double longitude;
    double latitude;

    assert(wspr_grid_center("MK68xm", &longitude, &latitude));
    assert(fabs(longitude - 73.958333) < 0.00001);
    assert(fabs(latitude - 18.520833) < 0.00001);
    assert(wspr_grid_center("MK68", &longitude, &latitude));
    assert(fabs(longitude - 73.0) < 0.00001);
    assert(fabs(latitude - 18.5) < 0.00001);
    assert(!wspr_grid_center("ZZ99", &longitude, &latitude));

    assert(wspr_hop_plan_parse(&candidates,
                               "80m,40m,30m,20m,17m,15m,12m,10m"));
    assert(wspr_advisor_parse_response(&candidates,
                                       "21\t4\t5\n28\t4\t5\n14\t2\t6\n"
                                       "7\t1\t45\n10\t1\t1\n",
                                       &ranked));
    assert(ranked.count == 4U);
    assert(strcmp(ranked.bands[0].name, "15m") == 0);
    assert(strcmp(ranked.bands[1].name, "10m") == 0);
    assert(strcmp(ranked.bands[2].name, "20m") == 0);
    assert(strcmp(ranked.bands[3].name, "40m") == 0);
    assert(!wspr_advisor_parse_response(&candidates, "14\t2\t6\n", &ranked));
    assert(!wspr_advisor_parse_response(&candidates, "invalid\n", &ranked));

    assert(ft8_hop_plan_parse(&candidates,
                              "80m,40m,30m,20m,17m,15m,12m,10m"));
    assert(ft8_advisor_format_url(url, sizeof(url), "MK68xm", &candidates));
    assert(strcmp(url,
                  "https://pskreporter.info/cgi-bin/psk-freq.pl?grid=MK&freq="
                  "3573000,7074000,10136000,14074000,18100000,21074000,"
                  "24915000,28074000") == 0);
    assert(ft8_advisor_parse_response(&candidates,
                                       "14074000 99 123\n", &recommended));
    assert(recommended.count == 1U);
    assert(strcmp(recommended.bands[0].name, "20m") == 0);
    assert(ft8_advisor_parse_response(
        &candidates,
        "21076000 51 23 0 2\n"
        "14076000 40 27 0 2\n"
        "# frequency score #spots #tx #rx\n",
        &recommended));
    assert(strcmp(recommended.bands[0].name, "15m") == 0);
    assert(ft8_advisor_parse_response(
        &candidates,
        "7039000 10 1 1 1\n14076000 9 6 0 1\n",
        &recommended));
    assert(strcmp(recommended.bands[0].name, "20m") == 0);
    assert(!ft8_advisor_parse_response(&candidates, "14095600 1\n",
                                        &recommended));
    assert(!ft8_advisor_parse_response(&candidates, "none\n", &recommended));
    assert(!ft8_advisor_format_url(url, 16U, "MK68xm", &candidates));

    assert(ft8_monitor_format_url(url, sizeof(url)));
    assert(strcmp(url,
                  "https://pskreporter.info/cgi-bin/pskquery5.pl?callback="
                  "doNothing&statistics=1&noactive=1&nolocator=1&"
                  "flowStartSeconds=-86400&callsign=ZZZZZ") == 0);
    assert(ft8_monitor_parse_response(
        &candidates,
        "doNothing({\"activeReceiver\":["
        "{\"callsign\":\"A\",\"bands\":\"20m,15m,6m\"},"
        "{\"callsign\":\"B\",\"bands\":\"20m,15m,40m\"},"
        "{\"callsign\":\"C\",\"bands\":\"20m,15m,40m,10m\"},"
        "{\"callsign\":\"D\",\"bands\":\"20m,40m,10m\"},"
        "{\"callsign\":\"E\",\"bands\":\"20m,17m\"},"
        "{\"callsign\":\"F\",\"frequency\":28074123}"
        "],\"rawReports\":[]});",
        &ranked));
    assert(ranked.count == 4U);
    assert(strcmp(ranked.bands[0].name, "20m") == 0);
    assert(strcmp(ranked.bands[1].name, "40m") == 0);
    assert(strcmp(ranked.bands[2].name, "15m") == 0);
    assert(strcmp(ranked.bands[3].name, "10m") == 0);
    assert(!ft8_monitor_parse_response(
        &candidates, "{\"activeReceiver\":[{\"bands\":\"20m\"}]}",
        &ranked));
    assert(!ft8_monitor_parse_response(&candidates, "{\"other\":[]}",
                                        &ranked));
    assert(!ft8_monitor_format_url(url, 16U));

    assert(pthread_mutex_init(&advisor.mutex, NULL) == 0);
    assert(pthread_cond_init(&advisor.condition, NULL) == 0);
    advisor.started = true;
    advisor.pending = candidates;
    advisor.update_ready = true;
    assert(wspr_band_advisor_wait(&advisor, &ranked, 0U));
    assert(ranked.count == candidates.count);
    assert(!wspr_band_advisor_take(&advisor, &ranked));
    assert(!wspr_band_advisor_wait(&advisor, &ranked, 0U));
    assert(pthread_cond_destroy(&advisor.condition) == 0);
    assert(pthread_mutex_destroy(&advisor.mutex) == 0);

    puts("band advisor: wspr.live, PSK monitor ranking, and FT8 fallback verified");
    return 0;
}
