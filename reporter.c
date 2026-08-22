#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reporter.h"

#define DEFAULT_WSPRNET_ENDPOINT "https://wsprnet.org/post"
#define MAX_RETRY_DELAY_SECONDS 900U

static bool url_encode(char *destination, size_t capacity, const char *source)
{
    static const char hexadecimal[] = "0123456789ABCDEF";
    size_t output = 0;

    for (size_t input = 0; source[input] != '\0'; input++) {
        unsigned char character = (unsigned char)source[input];
        if (isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            if (output + 1 >= capacity) {
                return false;
            }
            destination[output++] = (char)character;
        } else {
            if (output + 3 >= capacity) {
                return false;
            }
            destination[output++] = '%';
            destination[output++] = hexadecimal[character >> 4U];
            destination[output++] = hexadecimal[character & 0x0fU];
        }
    }
    if (output >= capacity) {
        return false;
    }
    destination[output] = '\0';
    return true;
}

int wspr_report_format_url(char *url,
                           size_t url_size,
                           const char *endpoint,
                           const char *version,
                           const struct wspr_report *report)
{
    char receiver_call[37];
    char receiver_grid[19];
    char transmitter_call[37];
    char transmitter_grid[19];
    char power[7];
    int length;

    if (url == NULL || endpoint == NULL || version == NULL || report == NULL ||
        !url_encode(receiver_call, sizeof(receiver_call), report->receiver_call) ||
        !url_encode(receiver_grid, sizeof(receiver_grid), report->receiver_grid) ||
        !url_encode(transmitter_call, sizeof(transmitter_call), report->transmitter_call) ||
        !url_encode(transmitter_grid, sizeof(transmitter_grid), report->transmitter_grid) ||
        !url_encode(power, sizeof(power), report->power)) {
        return -1;
    }

    length = snprintf(url, url_size,
                      "%s?function=wspr&rcall=%s&rgrid=%s&rqrg=%.6f"
                      "&date=%s&time=%s&sig=%.0f&dt=%.1f&drift=%.0f"
                      "&tqrg=%.6f&tcall=%s&tgrid=%s&dbm=%s&version=%s&mode=2",
                      endpoint, receiver_call, receiver_grid,
                      report->receiver_frequency_mhz, report->date, report->time,
                      report->snr, report->dt, report->drift,
                      report->transmitter_frequency_mhz, transmitter_call,
                      transmitter_grid, power, version);
    if (length < 0 || (size_t)length >= url_size) {
        return -1;
    }
    return 0;
}

static size_t discard_response(void *contents, size_t size, size_t count, void *context)
{
    (void)contents;
    (void)context;
    return size * count;
}

static enum wspr_upload_result upload_url(CURL *curl, const char *url, long *http_status)
{
    CURLcode result;

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "airspyhf-wsprd/0.8");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
    result = curl_easy_perform(curl);
    *http_status = 0;
    if (result != CURLE_OK) {
        fprintf(stderr, "WSPRnet upload failed: %s\n", curl_easy_strerror(result));
        return WSPR_UPLOAD_RETRY;
    }
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
    if (*http_status >= 200 && *http_status < 300) {
        return WSPR_UPLOAD_OK;
    }
    if (*http_status >= 400 && *http_status < 500 &&
        *http_status != 408 && *http_status != 425 && *http_status != 429) {
        return WSPR_UPLOAD_REJECTED;
    }
    return WSPR_UPLOAD_RETRY;
}

static enum wspr_upload_result curl_transport(const char *url,
                                               long *http_status,
                                               void *context)
{
    return upload_url(context, url, http_status);
}

static unsigned int retry_delay(unsigned int attempts)
{
    const char *configured = getenv("AIRSPYHF_WSPRD_RETRY_BASE_SECONDS");
    unsigned long configured_delay = configured == NULL ? 0 : strtoul(configured, NULL, 10);
    unsigned int delay = configured_delay >= 1 && configured_delay <= 60 ?
                         (unsigned int)configured_delay : 5U;

    for (unsigned int index = 1; index < attempts && delay < MAX_RETRY_DELAY_SECONDS;
         index++) {
        delay *= 2U;
        if (delay > MAX_RETRY_DELAY_SECONDS) {
            delay = MAX_RETRY_DELAY_SECONDS;
        }
    }
    return delay;
}

static void remove_head(struct wspr_reporter *reporter)
{
    reporter->head = (reporter->head + 1U) % reporter->capacity;
    reporter->count--;
}

static void *uploader_thread(void *context)
{
    struct wspr_reporter *reporter = context;
    CURL *curl = reporter->curl_handle;

    pthread_mutex_lock(&reporter->mutex);
    while (!reporter->stopping) {
        struct wspr_report_queue_item item;
        time_t now;

        while (reporter->count == 0 && !reporter->stopping) {
            pthread_cond_wait(&reporter->condition, &reporter->mutex);
        }
        if (reporter->stopping) {
            break;
        }

        now = time(NULL);
        if (reporter->queue[reporter->head].retry_at > now) {
            struct timespec deadline = {
                .tv_sec = reporter->queue[reporter->head].retry_at,
                .tv_nsec = 0
            };
            (void)pthread_cond_timedwait(&reporter->condition, &reporter->mutex,
                                         &deadline);
            continue;
        }

        item = reporter->queue[reporter->head];
        pthread_mutex_unlock(&reporter->mutex);
        long http_status = 0;
        enum wspr_upload_result result = reporter->transport(item.url, &http_status,
                                                              reporter->transport_context);
        pthread_mutex_lock(&reporter->mutex);

        if (reporter->count == 0 ||
            reporter->queue[reporter->head].sequence != item.sequence) {
            continue;
        }
        if (result == WSPR_UPLOAD_OK) {
            remove_head(reporter);
        } else if (result == WSPR_UPLOAD_REJECTED) {
            fprintf(stderr, "WSPRnet rejected spot with HTTP status %ld; dropping it\n",
                    http_status);
            remove_head(reporter);
        } else {
            unsigned int attempts = ++reporter->queue[reporter->head].attempts;
            unsigned int delay = retry_delay(attempts);
            reporter->queue[reporter->head].retry_at = time(NULL) + delay;
            fprintf(stderr, "WSPRnet spot retained in RAM; retry %u in %u seconds\n",
                    attempts, delay);
        }
    }
    pthread_mutex_unlock(&reporter->mutex);
    curl_easy_cleanup(curl);
    return NULL;
}

int wspr_reporter_start(struct wspr_reporter *reporter, const char *version)
{
    const char *endpoint;

    if (reporter == NULL || version == NULL) {
        return -1;
    }
    memset(reporter, 0, sizeof(*reporter));
    reporter->capacity = WSPR_REPORT_QUEUE_CAPACITY;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return -1;
    }
    reporter->queue = calloc(reporter->capacity, sizeof(*reporter->queue));
    if (reporter->queue == NULL) {
        free(reporter->queue);
        reporter->queue = NULL;
        curl_global_cleanup();
        return -1;
    }
    endpoint = getenv("AIRSPYHF_WSPRD_ENDPOINT");
    if (endpoint == NULL || endpoint[0] == '\0') {
        endpoint = DEFAULT_WSPRNET_ENDPOINT;
    }
    if (snprintf(reporter->endpoint, sizeof(reporter->endpoint), "%s", endpoint) >=
            (int)sizeof(reporter->endpoint) ||
        snprintf(reporter->version, sizeof(reporter->version), "%s", version) >=
            (int)sizeof(reporter->version)) {
        free(reporter->queue);
        reporter->queue = NULL;
        curl_global_cleanup();
        return -1;
    }
    if (pthread_mutex_init(&reporter->mutex, NULL) != 0) {
        free(reporter->queue);
        reporter->queue = NULL;
        curl_global_cleanup();
        return -1;
    }
    if (pthread_cond_init(&reporter->condition, NULL) != 0) {
        pthread_mutex_destroy(&reporter->mutex);
        free(reporter->queue);
        reporter->queue = NULL;
        curl_global_cleanup();
        return -1;
    }
    reporter->curl_handle = curl_easy_init();
    if (reporter->curl_handle == NULL) {
        pthread_cond_destroy(&reporter->condition);
        pthread_mutex_destroy(&reporter->mutex);
        free(reporter->queue);
        reporter->queue = NULL;
        curl_global_cleanup();
        return -1;
    }
    reporter->transport = curl_transport;
    reporter->transport_context = reporter->curl_handle;
    if (pthread_create(&reporter->thread, NULL, uploader_thread, reporter) != 0) {
        curl_easy_cleanup(reporter->curl_handle);
        reporter->curl_handle = NULL;
        pthread_cond_destroy(&reporter->condition);
        pthread_mutex_destroy(&reporter->mutex);
        free(reporter->queue);
        reporter->queue = NULL;
        curl_global_cleanup();
        return -1;
    }
    reporter->started = true;
    return 0;
}

bool wspr_reporter_enqueue(struct wspr_reporter *reporter,
                           const struct wspr_report *report)
{
    struct wspr_report_queue_item item = {0};
    size_t tail;

    if (reporter == NULL || report == NULL || !reporter->started ||
        wspr_report_format_url(item.url, sizeof(item.url), reporter->endpoint,
                               reporter->version, report) != 0) {
        return false;
    }

    pthread_mutex_lock(&reporter->mutex);
    if (reporter->count == reporter->capacity || reporter->stopping) {
        pthread_mutex_unlock(&reporter->mutex);
        fprintf(stderr, "WSPRnet RAM queue is full; spot could not be retained\n");
        return false;
    }
    item.sequence = ++reporter->next_sequence;
    tail = (reporter->head + reporter->count) % reporter->capacity;
    reporter->queue[tail] = item;
    reporter->count++;
    pthread_cond_signal(&reporter->condition);
    pthread_mutex_unlock(&reporter->mutex);
    return true;
}

size_t wspr_reporter_pending(struct wspr_reporter *reporter)
{
    size_t count;

    if (reporter == NULL || !reporter->started) {
        return 0;
    }
    pthread_mutex_lock(&reporter->mutex);
    count = reporter->count;
    pthread_mutex_unlock(&reporter->mutex);
    return count;
}

void wspr_reporter_set_transport(struct wspr_reporter *reporter,
                                 wspr_report_transport transport,
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

void wspr_reporter_stop(struct wspr_reporter *reporter)
{
    if (reporter == NULL || !reporter->started) {
        return;
    }
    pthread_mutex_lock(&reporter->mutex);
    reporter->stopping = true;
    pthread_cond_signal(&reporter->condition);
    pthread_mutex_unlock(&reporter->mutex);
    pthread_join(reporter->thread, NULL);
    pthread_cond_destroy(&reporter->condition);
    pthread_mutex_destroy(&reporter->mutex);
    free(reporter->queue);
    reporter->queue = NULL;
    reporter->curl_handle = NULL;
    reporter->started = false;
    curl_global_cleanup();
}
