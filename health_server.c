#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "health_server.h"

#define HEALTH_REQUEST_CAPACITY 1024U
#define HEALTH_JSON_CAPACITY 2048U

static void set_close_on_exec(int socket_fd)
{
    int flags = fcntl(socket_fd, F_GETFD);

    if (flags >= 0) {
        (void)fcntl(socket_fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

static bool send_all(int socket_fd, const char *data, size_t length)
{
    size_t sent = 0U;

    while (sent < length) {
        ssize_t result = send(socket_fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += (size_t)result;
    }
    return true;
}

static void send_plain_response(int socket_fd, int status, const char *reason)
{
    char response[256];
    int length = snprintf(response, sizeof(response),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: text/plain\r\n"
                          "Content-Length: 0\r\n"
                          "Cache-Control: no-store\r\n"
                          "Connection: close\r\n\r\n",
                          status, reason);
    if (length > 0 && (size_t)length < sizeof(response)) {
        (void)send_all(socket_fd, response, (size_t)length);
    }
}

static void send_health_response(struct wspr_health_server *server, int socket_fd)
{
    char json[HEALTH_JSON_CAPACITY];
    char response[HEALTH_JSON_CAPACITY + 384U];
    time_t now = time(NULL);
    size_t queue_depth = server->queue_depth == NULL ? 0U :
                         server->queue_depth(server->queue_context);
    bool healthy;
    int json_length;
    int response_length;
    double average_ft8lib;
    double average_jt9;

    pthread_mutex_lock(&server->mutex);
    healthy = server->clock_synchronized &&
              (strcmp(server->receiver_state, "waiting") == 0 ||
               strcmp(server->receiver_state, "capturing") == 0);
    average_ft8lib = server->frames_decoded_ft8lib == 0U ? 0.0 :
        (double)server->total_decodes_ft8lib /
        (double)server->frames_decoded_ft8lib;
    average_jt9 = server->frames_decoded_jt9 == 0U ? 0.0 :
        (double)server->total_decodes_jt9 /
        (double)server->frames_decoded_jt9;
    json_length = snprintf(
        json, sizeof(json),
        "{\n"
        "  \"healthy\": %s,\n"
        "  \"receiver_state\": \"%s\",\n"
        "  \"mode\": \"%s\",\n"
        "  \"clock_synchronized\": %s,\n"
        "  \"decoder_busy\": %s,\n"
        "  \"callsign\": \"%s\",\n"
        "  \"grid\": \"%s\",\n"
        "  \"band\": \"%s\",\n"
        "  \"dial_frequency_hz\": %u,\n"
        "  \"band_hopping\": %s,\n"
        "  \"hop_interval_seconds\": %u,\n"
        "  \"adaptive_hopping\": %s,\n"
        "  \"selected_bands\": %s,\n"
        "  \"exploration_slot\": %s,\n"
        "  \"reporting_enabled\": %s,\n"
        "  \"reporting_network\": \"%s\",\n"
        "  \"report_queue_depth\": %zu,\n"
        "  \"frames_decoded\": %llu,\n"
        "  \"spots_decoded\": %llu,\n"
        "  \"decoder_errors\": %llu,\n"
        "  \"last_decodes_ft8lib\": %u,\n"
        "  \"last_decodes_jt9\": %u,\n"
        "  \"total_decodes_ft8lib\": %llu,\n"
        "  \"total_decodes_jt9\": %llu,\n"
        "  \"frames_decoded_ft8lib\": %llu,\n"
        "  \"frames_decoded_jt9\": %llu,\n"
        "  \"average_decodes_ft8lib\": %.2f,\n"
        "  \"average_decodes_jt9\": %.2f,\n"
        "  \"airspy_overrun_samples\": %llu,\n"
        "  \"dsp_queue_overrun_samples\": %llu,\n"
        "  \"started_unix\": %lld,\n"
        "  \"uptime_seconds\": %lld,\n"
        "  \"last_decode_unix\": %lld,\n"
        "  \"last_spot_unix\": %lld,\n"
        "  \"version\": \"0.9\"\n"
        "}\n",
        healthy ? "true" : "false",
        server->receiver_state, server->mode,
        server->clock_synchronized ? "true" : "false",
        server->decoder_busy ? "true" : "false",
        server->callsign, server->grid, server->band,
        server->dial_frequency_hz,
        server->hopping_enabled ? "true" : "false",
        server->hop_interval_seconds,
        server->adaptive_hopping ? "true" : "false",
        server->selected_bands_json,
        server->exploration_slot ? "true" : "false",
        server->reporting_enabled ? "true" : "false",
        server->reporting_enabled ?
            (strcmp(server->mode, "FT8") == 0 ? "PSK Reporter" : "WSPRnet") :
            "disabled",
        queue_depth,
        (unsigned long long)server->frames_decoded,
        (unsigned long long)server->spots_decoded,
        (unsigned long long)server->decoder_errors,
        server->last_decodes_ft8lib,
        server->last_decodes_jt9,
        (unsigned long long)server->total_decodes_ft8lib,
        (unsigned long long)server->total_decodes_jt9,
        (unsigned long long)server->frames_decoded_ft8lib,
        (unsigned long long)server->frames_decoded_jt9,
        average_ft8lib,
        average_jt9,
        (unsigned long long)server->airspy_overrun_samples,
        (unsigned long long)server->dsp_queue_overrun_samples,
        (long long)server->started_unix,
        (long long)(now - server->started_unix),
        (long long)server->last_decode_unix,
        (long long)server->last_spot_unix);
    pthread_mutex_unlock(&server->mutex);

    if (json_length < 0 || (size_t)json_length >= sizeof(json)) {
        send_plain_response(socket_fd, 500, "Internal Server Error");
        return;
    }
    response_length = snprintf(
        response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n\r\n%s",
        healthy ? 200 : 503, healthy ? "OK" : "Service Unavailable",
        json_length, json);
    if (response_length > 0 && (size_t)response_length < sizeof(response)) {
        (void)send_all(socket_fd, response, (size_t)response_length);
    }
}

static void handle_client(struct wspr_health_server *server, int client_fd)
{
    char request[HEALTH_REQUEST_CAPACITY];
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    ssize_t length;

    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    length = recv(client_fd, request, sizeof(request) - 1U, 0);
    if (length <= 0) {
        return;
    }
    request[length] = '\0';
    if (strncmp(request, "GET /health HTTP/1.0\r\n", 22U) == 0 ||
        strncmp(request, "GET /health HTTP/1.1\r\n", 22U) == 0) {
        send_health_response(server, client_fd);
    } else if (strncmp(request, "GET ", 4U) == 0) {
        send_plain_response(client_fd, 404, "Not Found");
    } else {
        send_plain_response(client_fd, 405, "Method Not Allowed");
    }
}

static void *health_thread(void *context)
{
    struct wspr_health_server *server = context;

    while (true) {
        struct pollfd descriptor = {.fd = server->listen_fd, .events = POLLIN};
        int poll_result = poll(&descriptor, 1, 500);

        pthread_mutex_lock(&server->mutex);
        bool stopping = server->stopping;
        pthread_mutex_unlock(&server->mutex);
        if (stopping) {
            break;
        }
        if (poll_result <= 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd >= 0) {
            set_close_on_exec(client_fd);
            handle_client(server, client_fd);
            close(client_fd);
        }
    }
    return NULL;
}

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
                             void *queue_context)
{
    struct sockaddr_in address = {0};
    socklen_t address_length = sizeof(address);
    int reuse = 1;

    if (server == NULL || bind_address == NULL || mode == NULL ||
        callsign == NULL || grid == NULL ||
        inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        return -1;
    }
    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->started_unix = time(NULL);
    server->reporting_enabled = reporting_enabled;
    server->hopping_enabled = hopping_enabled;
    server->hop_interval_seconds = hopping_enabled ?
        (strcmp(mode, "FT8") == 0 ? FT8_HOP_DWELL_SECONDS :
                                    WSPR_HOP_DWELL_SECONDS) : 0U;
    server->adaptive_hopping = adaptive_hopping;
    (void)snprintf(server->selected_bands_json,
                   sizeof(server->selected_bands_json), "[]");
    server->queue_depth = queue_depth;
    server->queue_context = queue_context;
    if (snprintf(server->bind_address, sizeof(server->bind_address), "%s", bind_address) >=
            (int)sizeof(server->bind_address) ||
        snprintf(server->mode, sizeof(server->mode), "%s", mode) >=
            (int)sizeof(server->mode) ||
        snprintf(server->callsign, sizeof(server->callsign), "%s", callsign) >=
            (int)sizeof(server->callsign) ||
        snprintf(server->grid, sizeof(server->grid), "%s", grid) >=
            (int)sizeof(server->grid) ||
        snprintf(server->receiver_state, sizeof(server->receiver_state), "starting") >=
            (int)sizeof(server->receiver_state) ||
        pthread_mutex_init(&server->mutex, NULL) != 0) {
        return -1;
    }

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        pthread_mutex_destroy(&server->mutex);
        return -1;
    }
    set_close_on_exec(server->listen_fd);
    (void)setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (bind(server->listen_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listen_fd, 4) != 0 ||
        getsockname(server->listen_fd, (struct sockaddr *)&address, &address_length) != 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        pthread_mutex_destroy(&server->mutex);
        return -1;
    }
    server->port = ntohs(address.sin_port);
    if (pthread_create(&server->thread, NULL, health_thread, server) != 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        pthread_mutex_destroy(&server->mutex);
        return -1;
    }
    server->started = true;
    return 0;
}

void wspr_health_set_receiver_state(struct wspr_health_server *server,
                                    const char *state)
{
    if (server == NULL || !server->started || state == NULL) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    (void)snprintf(server->receiver_state, sizeof(server->receiver_state), "%s", state);
    pthread_mutex_unlock(&server->mutex);
}

void wspr_health_set_clock(struct wspr_health_server *server, bool synchronized)
{
    if (server == NULL || !server->started) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    server->clock_synchronized = synchronized;
    pthread_mutex_unlock(&server->mutex);
}

void wspr_health_set_tuning(struct wspr_health_server *server,
                            const char *band,
                            uint32_t dial_frequency_hz,
                            bool exploration_slot)
{
    if (server == NULL || !server->started || band == NULL) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    (void)snprintf(server->band, sizeof(server->band), "%s", band);
    server->dial_frequency_hz = dial_frequency_hz;
    server->exploration_slot = exploration_slot;
    pthread_mutex_unlock(&server->mutex);
}

void wspr_health_set_selected_bands(struct wspr_health_server *server,
                                    const struct wspr_hop_plan *plan)
{
    char json[sizeof(server->selected_bands_json)];
    size_t used = 0U;

    if (server == NULL || !server->started || plan == NULL ||
        plan->count > WSPR_MAX_HOP_BANDS) {
        return;
    }
    json[used++] = '[';
    for (size_t index = 0U; index < plan->count; index++) {
        int length = snprintf(json + used, sizeof(json) - used,
                              "%s\"%s\"", index == 0U ? "" : ", ",
                              plan->bands[index].name);

        if (length < 0 || (size_t)length >= sizeof(json) - used) {
            return;
        }
        used += (size_t)length;
    }
    if (used + 2U > sizeof(json)) {
        return;
    }
    json[used++] = ']';
    json[used] = '\0';

    pthread_mutex_lock(&server->mutex);
    (void)snprintf(server->selected_bands_json,
                   sizeof(server->selected_bands_json), "%s", json);
    pthread_mutex_unlock(&server->mutex);
}

void wspr_health_set_decoder_busy(struct wspr_health_server *server, bool busy)
{
    if (server == NULL || !server->started) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    server->decoder_busy = busy;
    pthread_mutex_unlock(&server->mutex);
}

void wspr_health_record_decode(struct wspr_health_server *server,
                               bool success,
                               uint32_t spot_count,
                               uint64_t airspy_overrun_samples,
                               uint64_t dsp_queue_overrun_samples)
{
    if (server == NULL || !server->started) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    server->last_decode_unix = time(NULL);
    server->airspy_overrun_samples += airspy_overrun_samples;
    server->dsp_queue_overrun_samples += dsp_queue_overrun_samples;
    if (success) {
        server->frames_decoded++;
        server->spots_decoded += spot_count;
        if (spot_count != 0U) {
            server->last_spot_unix = server->last_decode_unix;
        }
    } else {
        server->decoder_errors++;
    }
    pthread_mutex_unlock(&server->mutex);
}

void wspr_health_record_ft8_decoders(struct wspr_health_server *server,
                                     bool ft8lib_success,
                                     uint32_t ft8lib_count,
                                     bool jt9_success,
                                     uint32_t jt9_count)
{
    if (server == NULL || !server->started) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    server->last_decodes_ft8lib = ft8lib_success ? ft8lib_count : 0U;
    server->last_decodes_jt9 = jt9_success ? jt9_count : 0U;
    if (ft8lib_success) {
        server->frames_decoded_ft8lib++;
        server->total_decodes_ft8lib += ft8lib_count;
    }
    if (jt9_success) {
        server->frames_decoded_jt9++;
        server->total_decodes_jt9 += jt9_count;
    }
    pthread_mutex_unlock(&server->mutex);
}

void wspr_health_server_stop(struct wspr_health_server *server)
{
    if (server == NULL || !server->started) {
        return;
    }
    pthread_mutex_lock(&server->mutex);
    server->stopping = true;
    pthread_mutex_unlock(&server->mutex);
    pthread_join(server->thread, NULL);
    close(server->listen_fd);
    server->listen_fd = -1;
    pthread_mutex_destroy(&server->mutex);
    server->started = false;
}
