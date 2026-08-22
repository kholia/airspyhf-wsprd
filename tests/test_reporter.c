#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "reporter.h"

struct test_transport {
    unsigned int requests;
};

static enum wspr_upload_result retry_once(const char *url,
                                          long *http_status,
                                          void *context)
{
    struct test_transport *transport = context;

    assert(strstr(url, "function=wspr") != NULL);
    transport->requests++;
    if (transport->requests == 1) {
        *http_status = 503;
        return WSPR_UPLOAD_RETRY;
    }
    *http_status = 204;
    return WSPR_UPLOAD_OK;
}

int main(void)
{
    struct wspr_report report = {
        .receiver_call = "N0CALL/P",
        .receiver_grid = "AA00aa",
        .date = "150426",
        .time = "0918",
        .transmitter_call = "G8VDQ",
        .transmitter_grid = "IO91",
        .power = "37",
        .receiver_frequency_mhz = 14.095600,
        .transmitter_frequency_mhz = 14.097112,
        .snr = -21.0f,
        .dt = 0.4f,
        .drift = -1.0f
    };
    struct wspr_reporter reporter;
    struct test_transport transport = {0};
    char url[WSPR_REPORT_URL_SIZE];

    assert(wspr_report_format_url(url, sizeof(url), "https://wsprnet.org/post",
                                  "airhf-060", &report) == 0);
    assert(strstr(url, "rcall=N0CALL%2FP") != NULL);
    assert(strstr(url, "rqrg=14.095600") != NULL);
    assert(strstr(url, "tqrg=14.097112") != NULL);
    assert(strstr(url, "drift=-1") != NULL);
    assert(strstr(url, "mode=2") != NULL);

    report.receiver_frequency_mhz = 7.038600;
    report.transmitter_frequency_mhz = 7.040112;
    assert(wspr_report_format_url(url, sizeof(url), "https://wsprnet.org/post",
                                  "airhf-060", &report) == 0);
    assert(strstr(url, "rqrg=7.038600") != NULL);
    assert(strstr(url, "tqrg=7.040112") != NULL);

    assert(setenv("AIRSPYHF_WSPRD_RETRY_BASE_SECONDS", "1", 1) == 0);
    assert(wspr_reporter_start(&reporter, "airhf-test") == 0);
    wspr_reporter_set_transport(&reporter, retry_once, &transport);
    assert(wspr_reporter_enqueue(&reporter, &report));
    for (unsigned int attempt = 0;
         attempt < 50 && wspr_reporter_pending(&reporter) != 0; attempt++) {
        usleep(100000);
    }
    assert(wspr_reporter_pending(&reporter) == 0);
    wspr_reporter_stop(&reporter);
    assert(transport.requests == 2);
    puts("reporter: URL fields and RAM queue HTTP retry verified");
    return 0;
}
