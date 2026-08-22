#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "psk_reporter.h"

#define DEFAULT_PSK_REPORTER_HOST "report.pskreporter.info"
#define DEFAULT_PSK_REPORTER_PORT "4739"
#define PSK_REPORT_FLUSH_SECONDS 300
#define PSK_REPORT_RETRY_SECONDS 60
#define PSK_REPORT_TEMPLATE_SECONDS 3600
#define PSK_REPORT_IMMEDIATE_COUNT 40U

/* Official PSK Reporter IPFIX templates. The sender template follows the
 * SunshineFT8 reference and carries callsign, frequency, SNR, mode, source,
 * and transmission time. */
static const uint8_t receiver_template[] = {
    0x00, 0x03, 0x00, 0x24, 0x99, 0x92, 0x00, 0x03, 0x00, 0x01,
    0x80, 0x02, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
    0x80, 0x04, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
    0x80, 0x08, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
    0x00, 0x00
};

static const uint8_t sender_template[] = {
    0x00, 0x02, 0x00, 0x34, 0x99, 0x93, 0x00, 0x06,
    0x80, 0x01, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
    0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8f,
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8f,
    0x80, 0x0a, 0xff, 0xff, 0x00, 0x00, 0x76, 0x8f,
    0x80, 0x0b, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8f,
    0x00, 0x96, 0x00, 0x04
};

static size_t align_four(size_t value)
{
    return (value + 3U) & ~(size_t)3U;
}

static void put_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8U);
    destination[1] = (uint8_t)value;
}

static void put_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static bool valid_text_length(const char *text, size_t limit)
{
    return text != NULL && text[0] != '\0' && strlen(text) <= limit;
}

static uint8_t *put_string(uint8_t *destination, const char *text)
{
    size_t length = strlen(text);

    destination[0] = (uint8_t)length;
    memcpy(destination + 1U, text, length);
    return destination + 1U + length;
}

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
                              size_t *packet_size)
{
    size_t receiver_size;
    size_t sender_size = 4U;
    size_t total_size;
    uint8_t *position;

    if (packet == NULL || reports == NULL || report_count == 0U ||
        packet_size == NULL || !valid_text_length(receiver_call, 12U) ||
        !valid_text_length(receiver_grid, 6U) ||
        !valid_text_length(software, 63U) || export_time < 0) {
        return -1;
    }
    receiver_size = align_four(4U + 1U + strlen(receiver_call) +
                               1U + strlen(receiver_grid) +
                               1U + strlen(software));
    for (size_t index = 0U; index < report_count; index++) {
        if (!valid_text_length(reports[index].sender_call, 12U) ||
            reports[index].frequency_hz == 0U || reports[index].flow_start < 0) {
            return -1;
        }
        sender_size += 1U + strlen(reports[index].sender_call) + 4U + 1U +
                       1U + 3U + 1U + 4U;
    }
    sender_size = align_four(sender_size);
    total_size = 16U + receiver_size + sender_size;
    if (include_templates) {
        total_size += sizeof(receiver_template) + sizeof(sender_template);
    }
    if (total_size > capacity || total_size > UINT16_MAX) {
        return -1;
    }

    memset(packet, 0, total_size);
    packet[0] = 0x00;
    packet[1] = 0x0a;
    put_u16(packet + 2U, (uint16_t)total_size);
    put_u32(packet + 4U, (uint32_t)export_time);
    put_u32(packet + 8U, sequence);
    put_u32(packet + 12U, observation_id);
    position = packet + 16U;

    if (include_templates) {
        memcpy(position, receiver_template, sizeof(receiver_template));
        position += sizeof(receiver_template);
        memcpy(position, sender_template, sizeof(sender_template));
        position += sizeof(sender_template);
    }

    position[0] = 0x99;
    position[1] = 0x92;
    put_u16(position + 2U, (uint16_t)receiver_size);
    position += 4U;
    position = put_string(position, receiver_call);
    position = put_string(position, receiver_grid);
    position = put_string(position, software);
    position = packet + total_size - sender_size;

    position[0] = 0x99;
    position[1] = 0x93;
    put_u16(position + 2U, (uint16_t)sender_size);
    position += 4U;
    for (size_t index = 0U; index < report_count; index++) {
        position = put_string(position, reports[index].sender_call);
        put_u32(position, reports[index].frequency_hz);
        position += 4U;
        *position++ = (uint8_t)reports[index].snr;
        position = put_string(position, "FT8");
        *position++ = 1U;
        put_u32(position, (uint32_t)reports[index].flow_start);
        position += 4U;
    }
    *packet_size = total_size;
    return 0;
}

static int connect_reporter(struct psk_reporter *reporter)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(reporter->hostname, reporter->port, &hints, &addresses) != 0) {
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        int socket_fd = socket(address->ai_family, address->ai_socktype,
                               address->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        int flags = fcntl(socket_fd, F_GETFD);
        if (flags >= 0) {
            (void)fcntl(socket_fd, F_SETFD, flags | FD_CLOEXEC);
        }
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            reporter->connected_fd = socket_fd;
            break;
        }
        close(socket_fd);
    }
    freeaddrinfo(addresses);
    return reporter->connected_fd >= 0 ? 0 : -1;
}

static bool udp_transport(const uint8_t *packet, size_t packet_size, void *context)
{
    struct psk_reporter *reporter = context;
    ssize_t sent;

    if (reporter->connected_fd < 0 && connect_reporter(reporter) != 0) {
        return false;
    }
    do {
        sent = send(reporter->connected_fd, packet, packet_size, 0);
    } while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)packet_size) {
        close(reporter->connected_fd);
        reporter->connected_fd = -1;
        return false;
    }
    return true;
}

static uint32_t session_identifier(void)
{
    uint32_t value = (uint32_t)time(NULL) ^ (uint32_t)getpid() ^ 0x9e3779b9U;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    return value == 0U ? 1U : value;
}

static void mark_recent(struct psk_reporter *reporter,
                        const struct psk_report *reports,
                        size_t report_count,
                        time_t sent_at)
{
    for (size_t index = 0U; index < report_count; index++) {
        size_t slot = 0U;
        time_t oldest = reporter->recent[0].sent_at;

        for (size_t recent = 0U; recent < 256U; recent++) {
            if (strcmp(reporter->recent[recent].callsign,
                       reports[index].sender_call) == 0 ||
                reporter->recent[recent].callsign[0] == '\0') {
                slot = recent;
                break;
            }
            if (reporter->recent[recent].sent_at < oldest) {
                oldest = reporter->recent[recent].sent_at;
                slot = recent;
            }
        }
        (void)snprintf(reporter->recent[slot].callsign,
                       sizeof(reporter->recent[slot].callsign), "%s",
                       reports[index].sender_call);
        reporter->recent[slot].sent_at = sent_at;
    }
}

static size_t packet_report_count(struct psk_reporter *reporter,
                                  bool include_templates,
                                  uint8_t *packet,
                                  size_t *packet_size,
                                  time_t now)
{
    size_t count = reporter->count < PSK_REPORT_IMMEDIATE_COUNT ?
                   reporter->count : PSK_REPORT_IMMEDIATE_COUNT;

    while (count > 0U &&
           psk_reporter_build_packet(packet, PSK_REPORT_PACKET_CAPACITY,
                                     reporter->receiver_call,
                                     reporter->receiver_grid,
                                     reporter->software, reporter->queue, count,
                                     reporter->sequence,
                                     reporter->observation_id, now,
                                     include_templates, packet_size) != 0) {
        count--;
    }
    return count;
}

static void *reporter_thread(void *context)
{
    struct psk_reporter *reporter = context;
    uint8_t packet[PSK_REPORT_PACKET_CAPACITY];

    pthread_mutex_lock(&reporter->mutex);
    while (!reporter->stopping) {
        time_t now = time(NULL);

        while (!reporter->stopping && reporter->count < PSK_REPORT_IMMEDIATE_COUNT &&
               now < reporter->next_flush) {
            struct timespec deadline = {.tv_sec = reporter->next_flush, .tv_nsec = 0};
            (void)pthread_cond_timedwait(&reporter->condition, &reporter->mutex,
                                         &deadline);
            now = time(NULL);
        }
        if (reporter->stopping || reporter->count == 0U) {
            if (reporter->count == 0U) {
                reporter->next_flush = now + PSK_REPORT_FLUSH_SECONDS +
                                       (time_t)(reporter->observation_id % 31U);
            }
            continue;
        }

        bool templates = reporter->packet_count < 3U ||
                         now - reporter->templates_last_sent >=
                             PSK_REPORT_TEMPLATE_SECONDS;
        size_t packet_size = 0U;
        size_t report_count = packet_report_count(reporter, templates, packet,
                                                  &packet_size, now);
        if (report_count == 0U) {
            fprintf(stderr, "PSK Reporter could not encode queued spots; dropping one\n");
            memmove(reporter->queue, reporter->queue + 1U,
                    (reporter->count - 1U) * sizeof(*reporter->queue));
            reporter->count--;
            continue;
        }

        psk_report_transport transport = reporter->transport;
        void *transport_context = reporter->transport_context;
        pthread_mutex_unlock(&reporter->mutex);
        bool sent = transport(packet, packet_size, transport_context);
        pthread_mutex_lock(&reporter->mutex);
        now = time(NULL);
        if (!sent) {
            fprintf(stderr,
                    "PSK Reporter UDP send failed; %zu spots retained in RAM\n",
                    reporter->count);
            /* A reconnect can change the UDP source port, so resend templates. */
            reporter->packet_count = 0U;
            reporter->templates_last_sent = 0;
            reporter->next_flush = now + PSK_REPORT_RETRY_SECONDS;
            continue;
        }
        mark_recent(reporter, reporter->queue, report_count, now);
        reporter->sequence += (uint32_t)report_count;
        reporter->packet_count++;
        if (templates) {
            reporter->templates_last_sent = now;
        }
        memmove(reporter->queue, reporter->queue + report_count,
                (reporter->count - report_count) * sizeof(*reporter->queue));
        reporter->count -= report_count;
        reporter->next_flush = now + PSK_REPORT_FLUSH_SECONDS +
                               (time_t)(reporter->observation_id % 31U);
    }
    pthread_mutex_unlock(&reporter->mutex);
    return NULL;
}

int psk_reporter_start(struct psk_reporter *reporter,
                       const char *receiver_call,
                       const char *receiver_grid,
                       const char *software)
{
    const char *hostname;
    const char *port;

    if (reporter == NULL || !valid_text_length(receiver_call, 12U) ||
        !valid_text_length(receiver_grid, 6U) ||
        !valid_text_length(software, 63U)) {
        return -1;
    }
    memset(reporter, 0, sizeof(*reporter));
    reporter->connected_fd = -1;
    reporter->queue = calloc(PSK_REPORT_QUEUE_CAPACITY, sizeof(*reporter->queue));
    if (reporter->queue == NULL) {
        return -1;
    }
    hostname = getenv("AIRSPYHF_WSPRD_PSK_REPORTER_HOST");
    port = getenv("AIRSPYHF_WSPRD_PSK_REPORTER_PORT");
    if (hostname == NULL || hostname[0] == '\0') {
        hostname = DEFAULT_PSK_REPORTER_HOST;
    }
    if (port == NULL || port[0] == '\0') {
        port = DEFAULT_PSK_REPORTER_PORT;
    }
    if (snprintf(reporter->receiver_call, sizeof(reporter->receiver_call), "%s",
                 receiver_call) >= (int)sizeof(reporter->receiver_call) ||
        snprintf(reporter->receiver_grid, sizeof(reporter->receiver_grid), "%s",
                 receiver_grid) >= (int)sizeof(reporter->receiver_grid) ||
        snprintf(reporter->software, sizeof(reporter->software), "%s", software) >=
            (int)sizeof(reporter->software) ||
        snprintf(reporter->hostname, sizeof(reporter->hostname), "%s", hostname) >=
            (int)sizeof(reporter->hostname) ||
        snprintf(reporter->port, sizeof(reporter->port), "%s", port) >=
            (int)sizeof(reporter->port) ||
        pthread_mutex_init(&reporter->mutex, NULL) != 0) {
        free(reporter->queue);
        reporter->queue = NULL;
        return -1;
    }
    if (pthread_cond_init(&reporter->condition, NULL) != 0) {
        pthread_mutex_destroy(&reporter->mutex);
        free(reporter->queue);
        reporter->queue = NULL;
        return -1;
    }
    reporter->observation_id = session_identifier();
    reporter->next_flush = time(NULL) + PSK_REPORT_FLUSH_SECONDS +
                           (time_t)(reporter->observation_id % 31U);
    reporter->transport = udp_transport;
    reporter->transport_context = reporter;
    if (pthread_create(&reporter->thread, NULL, reporter_thread, reporter) != 0) {
        pthread_cond_destroy(&reporter->condition);
        pthread_mutex_destroy(&reporter->mutex);
        free(reporter->queue);
        reporter->queue = NULL;
        return -1;
    }
    reporter->started = true;
    return 0;
}

bool psk_reporter_enqueue(struct psk_reporter *reporter,
                          const struct psk_report *report)
{
    time_t now = time(NULL);

    if (reporter == NULL || report == NULL || !reporter->started ||
        !valid_text_length(report->sender_call, 12U) ||
        report->frequency_hz == 0U || report->flow_start < 0) {
        return false;
    }
    pthread_mutex_lock(&reporter->mutex);
    for (size_t index = 0U; index < 256U; index++) {
        if (strcmp(reporter->recent[index].callsign, report->sender_call) == 0 &&
            now - reporter->recent[index].sent_at < PSK_REPORT_FLUSH_SECONDS) {
            pthread_mutex_unlock(&reporter->mutex);
            return true;
        }
    }
    for (size_t index = 0U; index < reporter->count; index++) {
        if (strcmp(reporter->queue[index].sender_call, report->sender_call) == 0) {
            if (report->snr > reporter->queue[index].snr) {
                reporter->queue[index] = *report;
            }
            pthread_mutex_unlock(&reporter->mutex);
            return true;
        }
    }
    if (reporter->count >= PSK_REPORT_QUEUE_CAPACITY || reporter->stopping) {
        pthread_mutex_unlock(&reporter->mutex);
        fprintf(stderr, "PSK Reporter RAM queue is full; spot was not retained\n");
        return false;
    }
    reporter->queue[reporter->count++] = *report;
    if (reporter->count >= PSK_REPORT_IMMEDIATE_COUNT) {
        pthread_cond_signal(&reporter->condition);
    }
    pthread_mutex_unlock(&reporter->mutex);
    return true;
}

size_t psk_reporter_pending(struct psk_reporter *reporter)
{
    size_t count;

    if (reporter == NULL || !reporter->started) {
        return 0U;
    }
    pthread_mutex_lock(&reporter->mutex);
    count = reporter->count;
    pthread_mutex_unlock(&reporter->mutex);
    return count;
}

void psk_reporter_set_transport(struct psk_reporter *reporter,
                                psk_report_transport transport,
                                void *context)
{
    if (reporter == NULL || !reporter->started || transport == NULL) {
        return;
    }
    pthread_mutex_lock(&reporter->mutex);
    reporter->transport = transport;
    reporter->transport_context = context;
    pthread_mutex_unlock(&reporter->mutex);
}

void psk_reporter_stop(struct psk_reporter *reporter)
{
    if (reporter == NULL || !reporter->started) {
        return;
    }
    pthread_mutex_lock(&reporter->mutex);
    reporter->stopping = true;
    pthread_cond_signal(&reporter->condition);
    pthread_mutex_unlock(&reporter->mutex);
    pthread_join(reporter->thread, NULL);
    if (reporter->connected_fd >= 0) {
        close(reporter->connected_fd);
    }
    pthread_cond_destroy(&reporter->condition);
    pthread_mutex_destroy(&reporter->mutex);
    free(reporter->queue);
    reporter->queue = NULL;
    reporter->started = false;
}
