#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define WSPR_REPORT_URL_SIZE 768U
#define WSPR_REPORT_QUEUE_CAPACITY 4096U

enum wspr_upload_result {
    WSPR_UPLOAD_OK,
    WSPR_UPLOAD_RETRY,
    WSPR_UPLOAD_REJECTED
};

typedef enum wspr_upload_result (*wspr_report_transport)(const char *url,
                                                          long *http_status,
                                                          void *context);

struct wspr_report {
    char receiver_call[13];
    char receiver_grid[7];
    char date[7];
    char time[5];
    char transmitter_call[13];
    char transmitter_grid[7];
    char power[3];
    double receiver_frequency_mhz;
    double transmitter_frequency_mhz;
    float snr;
    float dt;
    float drift;
};

struct wspr_report_queue_item {
    char url[WSPR_REPORT_URL_SIZE];
    uint64_t sequence;
    unsigned int attempts;
    time_t retry_at;
};

struct wspr_reporter {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct wspr_report_queue_item *queue;
    size_t head;
    size_t count;
    size_t capacity;
    uint64_t next_sequence;
    bool stopping;
    bool started;
    void *curl_handle;
    wspr_report_transport transport;
    void *transport_context;
    char endpoint[256];
    char version[32];
};

int wspr_report_format_url(char *url,
                           size_t url_size,
                           const char *endpoint,
                           const char *version,
                           const struct wspr_report *report);
int wspr_reporter_start(struct wspr_reporter *reporter, const char *version);
bool wspr_reporter_enqueue(struct wspr_reporter *reporter,
                           const struct wspr_report *report);
size_t wspr_reporter_pending(struct wspr_reporter *reporter);
void wspr_reporter_set_transport(struct wspr_reporter *reporter,
                                 wspr_report_transport transport,
                                 void *context);
void wspr_reporter_stop(struct wspr_reporter *reporter);
