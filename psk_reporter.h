#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define PSK_REPORT_QUEUE_CAPACITY 512U
#define PSK_REPORT_PACKET_CAPACITY 1400U

struct psk_report {
    char sender_call[13];
    uint32_t frequency_hz;
    int8_t snr;
    time_t flow_start;
};

typedef bool (*psk_report_transport)(const uint8_t *packet,
                                     size_t packet_size,
                                     void *context);

struct psk_recent_sender {
    char callsign[13];
    time_t sent_at;
};

struct psk_reporter {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct psk_report *queue;
    size_t count;
    uint32_t sequence;
    uint32_t observation_id;
    uint32_t packet_count;
    time_t templates_last_sent;
    time_t next_flush;
    bool stopping;
    bool started;
    int connected_fd;
    char receiver_call[13];
    char receiver_grid[7];
    char software[64];
    char hostname[256];
    char port[8];
    struct psk_recent_sender recent[256];
    psk_report_transport transport;
    void *transport_context;
};

int psk_reporter_build_packet(uint8_t *packet,
                              size_t capacity,
                              const char *receiver_call,
                              const char *receiver_grid,
                              const char *software,
                              const struct psk_report *reports,
                              size_t report_count,
                              uint32_t sequence,
                              uint32_t observation_id,
                              time_t export_time,
                              bool include_templates,
                              size_t *packet_size);
int psk_reporter_start(struct psk_reporter *reporter,
                       const char *receiver_call,
                       const char *receiver_grid,
                       const char *software);
bool psk_reporter_enqueue(struct psk_reporter *reporter,
                          const struct psk_report *report);
size_t psk_reporter_pending(struct psk_reporter *reporter);
void psk_reporter_set_transport(struct psk_reporter *reporter,
                                psk_report_transport transport,
                                void *context);
void psk_reporter_stop(struct psk_reporter *reporter);
