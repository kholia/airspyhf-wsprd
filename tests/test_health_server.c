#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "health_server.h"

static size_t queue_depth(void *context)
{
    return *(const size_t *)context;
}

static void fetch_health(uint16_t port, const char *request,
                         char *response, size_t response_size)
{
    struct sockaddr_in address = {0};
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    size_t used = 0U;

    assert(socket_fd >= 0);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(connect(socket_fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
    assert(send(socket_fd, request, strlen(request), 0) == (ssize_t)strlen(request));
    while (used + 1U < response_size) {
        ssize_t received = recv(socket_fd, response + used, response_size - used - 1U, 0);
        if (received <= 0) {
            break;
        }
        used += (size_t)received;
    }
    response[used] = '\0';
    close(socket_fd);
}

int main(void)
{
    struct wspr_health_server server;
    struct wspr_hop_plan selected;
    size_t pending = 3U;
    char response[4096];

    assert(wspr_health_server_start(&server, "127.0.0.1", 0, "WSPR",
                                    "VU3CER", "MK68xm",
                                    true, true, true, queue_depth, &pending) == 0);
    assert(server.port != 0U);
    wspr_health_set_clock(&server, true);
    wspr_health_set_receiver_state(&server, "capturing");
    wspr_health_set_tuning(&server, "15m", 21094600U, true);
    assert(wspr_hop_plan_parse(&selected, "20m,15m,40m,10m"));
    wspr_health_set_selected_bands(&server, &selected);
    wspr_health_set_decoder_busy(&server, true);
    wspr_health_record_decode(&server, true, 2U, 6144U, 0U);

    fetch_health(server.port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
                 response, sizeof(response));
    assert(strstr(response, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(response, "\"healthy\": true") != NULL);
    assert(strstr(response, "\"receiver_state\": \"capturing\"") != NULL);
    assert(strstr(response, "\"mode\": \"WSPR\"") != NULL);
    assert(strstr(response, "\"band\": \"15m\"") != NULL);
    assert(strstr(response, "\"dial_frequency_hz\": 21094600") != NULL);
    assert(strstr(response, "\"hop_interval_seconds\": 600") != NULL);
    assert(strstr(response,
                  "\"selected_bands\": [\"20m\", \"15m\", \"40m\", \"10m\"]") != NULL);
    assert(strstr(response, "\"exploration_slot\": true") != NULL);
    assert(strstr(response, "{\n  \"healthy\": true") != NULL);
    assert(strstr(response, "\"reporting_network\": \"WSPRnet\"") != NULL);
    assert(strstr(response, "\"report_queue_depth\": 3") != NULL);
    assert(strstr(response, "\"frames_decoded\": 1") != NULL);
    assert(strstr(response, "\"spots_decoded\": 2") != NULL);
    assert(strstr(response, "\"airspy_overrun_samples\": 6144") != NULL);

    wspr_health_set_clock(&server, false);
    fetch_health(server.port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
                 response, sizeof(response));
    assert(strstr(response, "HTTP/1.1 503 Service Unavailable") != NULL);
    assert(strstr(response, "\"healthy\": false") != NULL);
    wspr_health_set_clock(&server, true);

    fetch_health(server.port, "POST /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
                 response, sizeof(response));
    assert(strstr(response, "HTTP/1.1 405 Method Not Allowed") != NULL);
    fetch_health(server.port, "GET /other HTTP/1.1\r\nHost: localhost\r\n\r\n",
                 response, sizeof(response));
    assert(strstr(response, "HTTP/1.1 404 Not Found") != NULL);

    wspr_health_server_stop(&server);
    assert(wspr_health_server_start(&server, "127.0.0.1", 0, "FT8",
                                    "VU3CER", "MK68xm",
                                    true, true, true, queue_depth, &pending) == 0);
    assert(server.hop_interval_seconds == FT8_HOP_DWELL_SECONDS);
    wspr_health_set_clock(&server, true);
    wspr_health_set_receiver_state(&server, "capturing");
    wspr_health_record_ft8_decoders(&server, true, 4U, true, 6U);
    wspr_health_record_ft8_decoders(&server, true, 2U, true, 3U);
    fetch_health(server.port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n",
                 response, sizeof(response));
    assert(strstr(response, "\"last_decodes_ft8lib\": 2") != NULL);
    assert(strstr(response, "\"last_decodes_jt9\": 3") != NULL);
    assert(strstr(response, "\"total_decodes_ft8lib\": 6") != NULL);
    assert(strstr(response, "\"total_decodes_jt9\": 9") != NULL);
    assert(strstr(response, "\"frames_decoded_ft8lib\": 2") != NULL);
    assert(strstr(response, "\"frames_decoded_jt9\": 2") != NULL);
    assert(strstr(response, "\"average_decodes_ft8lib\": 3.00") != NULL);
    assert(strstr(response, "\"average_decodes_jt9\": 4.50") != NULL);
    wspr_health_server_stop(&server);
    puts("health server: bounded read-only HTTP JSON endpoint verified");
    return 0;
}
