#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "band_advisor.h"

#define ADVISOR_ENDPOINT "https://db1.wspr.live/"
#define FT8_ADVISOR_ENDPOINT "https://pskreporter.info/cgi-bin/psk-freq.pl"
#define FT8_MONITOR_ENDPOINT "https://pskreporter.info/cgi-bin/pskquery5.pl"
#define ADVISOR_INTERVAL_SECONDS 3600
#define ADVISOR_RESPONSE_CAPACITY 2048U
#define FT8_MONITOR_RESPONSE_CAPACITY (8U * 1024U * 1024U)
#define ADVISOR_RESULT_BANDS 4U

struct response_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

static size_t receive_response(char *contents, size_t size, size_t count, void *context)
{
    struct response_buffer *response = context;
    size_t bytes;

    if (size != 0U && count > SIZE_MAX / size) {
        return 0U;
    }
    bytes = size * count;
    if (response->data == NULL || response->capacity == 0U ||
        response->length >= response->capacity ||
        bytes > response->capacity - response->length - 1U) {
        return 0U;
    }
    memcpy(response->data + response->length, contents, bytes);
    response->length += bytes;
    response->data[response->length] = '\0';
    return bytes;
}

static bool json_array_end(const char *begin, const char **end)
{
    unsigned int depth = 1U;
    bool in_string = false;
    bool escaped = false;

    for (const char *position = begin; *position != '\0'; position++) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (*position == '\\') {
                escaped = true;
            } else if (*position == '"') {
                in_string = false;
            }
            continue;
        }
        if (*position == '"') {
            in_string = true;
        } else if (*position == '[') {
            depth++;
        } else if (*position == ']') {
            depth--;
            if (depth == 0U) {
                *end = position;
                return true;
            }
        }
    }
    return false;
}

static bool json_object_end(const char *begin, const char *limit, const char **end)
{
    unsigned int depth = 1U;
    bool in_string = false;
    bool escaped = false;

    for (const char *position = begin; position < limit; position++) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (*position == '\\') {
                escaped = true;
            } else if (*position == '"') {
                in_string = false;
            }
            continue;
        }
        if (*position == '"') {
            in_string = true;
        } else if (*position == '{') {
            depth++;
        } else if (*position == '}') {
            depth--;
            if (depth == 0U) {
                *end = position;
                return true;
            }
        }
    }
    return false;
}

static const char *json_object_value(const char *begin,
                                     const char *end,
                                     const char *wanted_key)
{
    const char *position = begin;
    size_t wanted_length = strlen(wanted_key);

    while (position < end) {
        const char *key;
        const char *key_end;

        if (*position++ != '"') {
            continue;
        }
        key = position;
        while (position < end && *position != '"') {
            if (*position++ == '\\' && position < end) {
                position++;
            }
        }
        if (position >= end) {
            return NULL;
        }
        key_end = position++;
        while (position < end && isspace((unsigned char)*position)) {
            position++;
        }
        if (position >= end || *position++ != ':') {
            continue;
        }
        while (position < end && isspace((unsigned char)*position)) {
            position++;
        }
        if ((size_t)(key_end - key) == wanted_length &&
            memcmp(key, wanted_key, wanted_length) == 0) {
            return position;
        }
    }
    return NULL;
}

static void count_monitor_bands(const struct wspr_hop_plan *candidates,
                                const char *value,
                                size_t length,
                                unsigned long counts[WSPR_MAX_HOP_BANDS])
{
    const char *field = value;
    const char *limit = value + length;

    while (field < limit) {
        const char *comma = memchr(field, ',', (size_t)(limit - field));
        const char *field_end = comma == NULL ? limit : comma;

        while (field < field_end && isspace((unsigned char)*field)) {
            field++;
        }
        while (field_end > field && isspace((unsigned char)field_end[-1])) {
            field_end--;
        }
        for (size_t index = 0U; index < candidates->count; index++) {
            size_t name_length = strlen(candidates->bands[index].name);

            if (name_length == (size_t)(field_end - field) &&
                memcmp(field, candidates->bands[index].name, name_length) == 0) {
                if (counts[index] != ULONG_MAX) {
                    counts[index]++;
                }
                break;
            }
        }
        field = comma == NULL ? limit : comma + 1;
    }
}

static void count_monitor_frequency(const struct wspr_hop_plan *candidates,
                                    unsigned long frequency,
                                    unsigned long counts[WSPR_MAX_HOP_BANDS])
{
    for (size_t index = 0U; index < candidates->count; index++) {
        unsigned long lower = (unsigned long)candidates->bands[index].database_band *
                              1000000UL;
        unsigned long upper = lower + 1000000UL;

        if (candidates->bands[index].database_band == 28) {
            upper = 30000000UL;
        }
        if (frequency >= lower && frequency < upper) {
            if (counts[index] != ULONG_MAX) {
                counts[index]++;
            }
            return;
        }
    }
}

bool wspr_grid_center(const char *grid, double *longitude, double *latitude)
{
    size_t length;
    char field_lon;
    char field_lat;

    if (grid == NULL || longitude == NULL || latitude == NULL) {
        return false;
    }
    length = strlen(grid);
    if (length != 4U && length != 6U) {
        return false;
    }
    field_lon = (char)toupper((unsigned char)grid[0]);
    field_lat = (char)toupper((unsigned char)grid[1]);
    if (field_lon < 'A' || field_lon > 'R' || field_lat < 'A' || field_lat > 'R' ||
        grid[2] < '0' || grid[2] > '9' || grid[3] < '0' || grid[3] > '9') {
        return false;
    }

    *longitude = -180.0 + (field_lon - 'A') * 20.0 + (grid[2] - '0') * 2.0;
    *latitude = -90.0 + (field_lat - 'A') * 10.0 + (grid[3] - '0');
    if (length == 6U) {
        char square_lon = (char)toupper((unsigned char)grid[4]);
        char square_lat = (char)toupper((unsigned char)grid[5]);
        if (square_lon < 'A' || square_lon > 'X' ||
            square_lat < 'A' || square_lat > 'X') {
            return false;
        }
        *longitude += (square_lon - 'A') * (5.0 / 60.0) + (2.5 / 60.0);
        *latitude += (square_lat - 'A') * (2.5 / 60.0) + (1.25 / 60.0);
    } else {
        *longitude += 1.0;
        *latitude += 0.5;
    }
    return true;
}

bool wspr_advisor_parse_response(const struct wspr_hop_plan *candidates,
                                 const char *response,
                                 struct wspr_hop_plan *ranked)
{
    const char *line = response;

    if (candidates == NULL || response == NULL || ranked == NULL) {
        return false;
    }
    memset(ranked, 0, sizeof(*ranked));
    while (*line != '\0' && ranked->count < ADVISOR_RESULT_BANDS) {
        const char *newline = strchr(line, '\n');
        size_t line_length = newline == NULL ? strlen(line) : (size_t)(newline - line);
        char row[128];
        int database_band;
        unsigned long long stations;
        unsigned long long spots;
        char extra;

        if (line_length == 0U || line_length >= sizeof(row)) {
            memset(ranked, 0, sizeof(*ranked));
            return false;
        }
        memcpy(row, line, line_length);
        row[line_length] = '\0';
        if (sscanf(row, "%d\t%llu\t%llu%c", &database_band,
                   &stations, &spots, &extra) != 3 || stations == 0U || spots == 0U) {
            memset(ranked, 0, sizeof(*ranked));
            return false;
        }
        line = newline == NULL ? line + line_length : newline + 1;
        for (size_t index = 0; index < candidates->count; index++) {
            if (candidates->bands[index].database_band == database_band) {
                bool duplicate = false;
                for (size_t selected = 0; selected < ranked->count; selected++) {
                    if (ranked->bands[selected].database_band == database_band) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    ranked->bands[ranked->count++] = candidates->bands[index];
                }
                break;
            }
        }
    }
    if (ranked->count < 2U) {
        memset(ranked, 0, sizeof(*ranked));
        return false;
    }
    return true;
}

bool ft8_advisor_parse_response(const struct wspr_hop_plan *candidates,
                                const char *response,
                                struct wspr_hop_plan *recommended)
{
    if (candidates == NULL || response == NULL || recommended == NULL) {
        return false;
    }
    memset(recommended, 0, sizeof(*recommended));
    while (*response != '\0') {
        const char *newline = strchr(response, '\n');
        size_t line_length = newline == NULL ? strlen(response) :
                                                (size_t)(newline - response);
        char row[128];
        char *end;
        unsigned long frequency;

        if (line_length >= sizeof(row)) {
            return false;
        }
        memcpy(row, response, line_length);
        row[line_length] = '\0';
        response = newline == NULL ? response + line_length : newline + 1;
        char *position = row;
        while (isspace((unsigned char)*position)) {
            position++;
        }
        if (*position == '\0' || *position == '#') {
            continue;
        }
        errno = 0;
        frequency = strtoul(position, &end, 10);
        if (errno != 0 || end == position || frequency == 0U ||
            frequency > UINT32_MAX ||
            (*end != '\0' && !isspace((unsigned char)*end))) {
            return false;
        }
        for (size_t index = 0U; index < candidates->count; index++) {
            uint32_t dial = candidates->bands[index].frequency_hz;
            bool exact_dial = frequency == dial;
            bool in_passband = frequency >= (unsigned long)dial + 200UL &&
                               frequency <= (unsigned long)dial + 3000UL;

            if (exact_dial || in_passband) {
                recommended->bands[0] = candidates->bands[index];
                recommended->count = 1U;
                return true;
            }
        }
    }
    return false;
}

bool ft8_monitor_parse_response(const struct wspr_hop_plan *candidates,
                                const char *response,
                                struct wspr_hop_plan *ranked)
{
    static const char active_receiver_key[] = "\"activeReceiver\"";
    unsigned long counts[WSPR_MAX_HOP_BANDS] = {0};
    bool selected[WSPR_MAX_HOP_BANDS] = {false};
    const char *position;
    const char *array_end;

    if (candidates == NULL || response == NULL || ranked == NULL ||
        candidates->count > WSPR_MAX_HOP_BANDS) {
        return false;
    }
    memset(ranked, 0, sizeof(*ranked));
    position = strstr(response, active_receiver_key);
    if (position == NULL) {
        return false;
    }
    position += sizeof(active_receiver_key) - 1U;
    while (isspace((unsigned char)*position)) {
        position++;
    }
    if (*position++ != ':') {
        return false;
    }
    while (isspace((unsigned char)*position)) {
        position++;
    }
    if (*position++ != '[' || !json_array_end(position, &array_end)) {
        return false;
    }

    while (position < array_end) {
        const char *object_end;
        const char *bands;

        while (position < array_end && *position != '{') {
            position++;
        }
        if (position >= array_end) {
            break;
        }
        if (!json_object_end(position + 1, array_end, &object_end)) {
            return false;
        }
        bands = json_object_value(position + 1, object_end, "bands");
        if (bands != NULL) {
            const char *bands_end;

            if (bands >= object_end || *bands++ != '"') {
                return false;
            }
            bands_end = memchr(bands, '"', (size_t)(object_end - bands));
            if (bands_end == NULL ||
                memchr(bands, '\\', (size_t)(bands_end - bands)) != NULL) {
                return false;
            }
            count_monitor_bands(candidates, bands,
                                (size_t)(bands_end - bands), counts);
        } else {
            const char *frequency_value = json_object_value(position + 1,
                                                             object_end,
                                                             "frequency");
            char *frequency_end;
            unsigned long frequency;

            if (frequency_value != NULL) {
                errno = 0;
                frequency = strtoul(frequency_value, &frequency_end, 10);
                if (errno != 0 || frequency_end == frequency_value ||
                    frequency_end > object_end) {
                    return false;
                }
                count_monitor_frequency(candidates, frequency, counts);
            }
        }
        position = object_end + 1;
    }

    while (ranked->count < ADVISOR_RESULT_BANDS) {
        size_t best = candidates->count;
        unsigned long best_count = 0U;

        for (size_t index = 0U; index < candidates->count; index++) {
            if (!selected[index] && counts[index] > best_count) {
                best = index;
                best_count = counts[index];
            }
        }
        if (best == candidates->count) {
            break;
        }
        selected[best] = true;
        ranked->bands[ranked->count++] = candidates->bands[best];
    }
    if (ranked->count < 2U) {
        memset(ranked, 0, sizeof(*ranked));
        return false;
    }
    return true;
}

bool ft8_advisor_format_url(char *url,
                            size_t capacity,
                            const char *grid,
                            const struct wspr_hop_plan *candidates)
{
    char frequencies[192] = "";
    char field[3];
    size_t used = 0U;

    if (url == NULL || capacity == 0U || grid == NULL ||
        candidates == NULL || candidates->count < 2U ||
        strlen(grid) < 2U) {
        return false;
    }
    field[0] = (char)toupper((unsigned char)grid[0]);
    field[1] = (char)toupper((unsigned char)grid[1]);
    field[2] = '\0';
    if (field[0] < 'A' || field[0] > 'R' ||
        field[1] < 'A' || field[1] > 'R') {
        return false;
    }
    for (size_t index = 0U; index < candidates->count; index++) {
        int length = snprintf(frequencies + used, sizeof(frequencies) - used,
                              "%s%u", index == 0U ? "" : ",",
                              candidates->bands[index].frequency_hz);
        if (length < 0 || (size_t)length >= sizeof(frequencies) - used) {
            return false;
        }
        used += (size_t)length;
    }
    int length = snprintf(url, capacity, "%s?grid=%s&freq=%s",
                          FT8_ADVISOR_ENDPOINT, field, frequencies);
    return length >= 0 && (size_t)length < capacity;
}

bool ft8_monitor_format_url(char *url, size_t capacity)
{
    int length;

    if (url == NULL || capacity == 0U) {
        return false;
    }
    length = snprintf(url, capacity,
                      "%s?callback=doNothing&statistics=1&noactive=1&nolocator=1&"
                      "flowStartSeconds=-86400&callsign=ZZZZZ",
                      FT8_MONITOR_ENDPOINT);
    return length >= 0 && (size_t)length < capacity;
}

static bool query_band_advice(const char *grid,
                              const struct wspr_hop_plan *candidates,
                              struct wspr_hop_plan *ranked)
{
    char response_data[ADVISOR_RESPONSE_CAPACITY] = {0};
    struct response_buffer response = {
        .data = response_data,
        .capacity = sizeof(response_data)
    };
    char band_ids[192] = "";
    char query[1024];
    char url[2048];
    double longitude;
    double latitude;
    CURL *curl;
    char *encoded_query;
    CURLcode result;
    size_t used = 0U;

    if (!wspr_grid_center(grid, &longitude, &latitude)) {
        return false;
    }
    for (size_t index = 0; index < candidates->count; index++) {
        int length = snprintf(band_ids + used, sizeof(band_ids) - used,
                              "%s%d", index == 0U ? "" : ",",
                              candidates->bands[index].database_band);
        if (length < 0 || (size_t)length >= sizeof(band_ids) - used) {
            return false;
        }
        used += (size_t)length;
    }
    if (snprintf(query, sizeof(query),
                 "SELECT band, uniqExact(tx_sign) AS stations, count() AS spots "
                 "FROM wspr.rx WHERE time > subtractHours(now(), 6) AND code = 1 "
                 "AND greatCircleDistance(rx_lon, rx_lat, %.6f, %.6f) <= 1500000 "
                 "AND band IN (%s) GROUP BY band "
                 "ORDER BY stations DESC, spots DESC FORMAT TabSeparated",
                 longitude, latitude, band_ids) >= (int)sizeof(query)) {
        return false;
    }

    curl = curl_easy_init();
    if (curl == NULL) {
        return false;
    }
    encoded_query = curl_easy_escape(curl, query, 0);
    if (encoded_query == NULL ||
        snprintf(url, sizeof(url), "%s?query=%s", ADVISOR_ENDPOINT, encoded_query) >=
            (int)sizeof(url)) {
        curl_free(encoded_query);
        curl_easy_cleanup(curl);
        return false;
    }
    curl_free(encoded_query);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "airspyhf-wsprd/0.8");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return result == CURLE_OK &&
           wspr_advisor_parse_response(candidates, response.data, ranked);
}

static bool query_ft8_band_advice(const char *grid,
                                  const struct wspr_hop_plan *candidates,
                                  struct wspr_hop_plan *recommended)
{
    char response_data[ADVISOR_RESPONSE_CAPACITY] = {0};
    struct response_buffer response = {
        .data = response_data,
        .capacity = sizeof(response_data)
    };
    char url[512];
    CURL *curl;
    CURLcode result;

    if (!ft8_advisor_format_url(url, sizeof(url), grid, candidates)) {
        return false;
    }
    curl = curl_easy_init();
    if (curl == NULL) {
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "airspyhf-wsprd/0.8");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return result == CURLE_OK &&
           ft8_advisor_parse_response(candidates, response.data, recommended);
}

static bool query_ft8_monitor_advice(const struct wspr_hop_plan *candidates,
                                     struct wspr_hop_plan *ranked)
{
    struct response_buffer response = {0};
    char url[512];
    CURL *curl;
    CURLcode result;
    bool parsed = false;

    response.data = malloc(FT8_MONITOR_RESPONSE_CAPACITY);
    if (response.data == NULL) {
        return false;
    }
    response.data[0] = '\0';
    response.capacity = FT8_MONITOR_RESPONSE_CAPACITY;
    if (!ft8_monitor_format_url(url, sizeof(url))) {
        free(response.data);
        return false;
    }
    curl = curl_easy_init();
    if (curl == NULL) {
        free(response.data);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "airspyhf-wsprd/0.8");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (result == CURLE_OK) {
        parsed = ft8_monitor_parse_response(candidates, response.data, ranked);
    }
    free(response.data);
    return parsed;
}

static bool wait_until(struct wspr_band_advisor *advisor, time_t target)
{
    struct timespec deadline = {.tv_sec = target, .tv_nsec = 0};

    pthread_mutex_lock(&advisor->mutex);
    while (!advisor->stopping && time(NULL) < target) {
        (void)pthread_cond_timedwait(&advisor->condition, &advisor->mutex, &deadline);
    }
    bool keep_running = !advisor->stopping;
    pthread_mutex_unlock(&advisor->mutex);
    return keep_running;
}

static void *advisor_thread(void *context)
{
    struct wspr_band_advisor *advisor = context;
    time_t now = time(NULL);
    time_t next_query = advisor->use_psk_reporter ?
                       now :
                       now + ((90 - now % 120 + 120) % 120);

    while (wait_until(advisor, next_query)) {
        struct wspr_hop_plan ranked;
        bool monitor_counts = false;
        bool available;

        if (advisor->use_psk_reporter) {
            monitor_counts = query_ft8_monitor_advice(&advisor->candidates,
                                                       &ranked);
            available = monitor_counts ||
                        query_ft8_band_advice(advisor->grid,
                                              &advisor->candidates, &ranked);
        } else {
            available = query_band_advice(advisor->grid,
                                          &advisor->candidates, &ranked);
        }
        if (available) {
            pthread_mutex_lock(&advisor->mutex);
            advisor->pending = ranked;
            advisor->update_ready = true;
            pthread_cond_broadcast(&advisor->condition);
            pthread_mutex_unlock(&advisor->mutex);
            fprintf(stderr, "%s selected %s:",
                    advisor->use_psk_reporter ?
                        (monitor_counts ? "PSK Reporter active monitors" :
                                          "PSK Reporter regional fallback") :
                        "wspr.live",
                    ranked.count == 1U ? "band" : "bands");
            for (size_t index = 0; index < ranked.count; index++) {
                fprintf(stderr, " %s", ranked.bands[index].name);
            }
            fprintf(stderr, " (ready for activation)\n");
        } else {
            fprintf(stderr, "%s band advice unavailable; keeping local schedule\n",
                    advisor->use_psk_reporter ? "PSK Reporter" : "wspr.live");
        }
        next_query += ADVISOR_INTERVAL_SECONDS;
        now = time(NULL);
        if (next_query <= now) {
            next_query = now + ADVISOR_INTERVAL_SECONDS;
        }
    }
    return NULL;
}

static int band_advisor_start(struct wspr_band_advisor *advisor,
                              const char *grid,
                              const struct wspr_hop_plan *candidates,
                              bool use_psk_reporter)
{
    if (advisor == NULL || grid == NULL || candidates == NULL || candidates->count < 2U) {
        return -1;
    }
    memset(advisor, 0, sizeof(*advisor));
    advisor->candidates = *candidates;
    advisor->use_psk_reporter = use_psk_reporter;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return -1;
    }
    if (snprintf(advisor->grid, sizeof(advisor->grid), "%s", grid) >=
        (int)sizeof(advisor->grid)) {
        curl_global_cleanup();
        return -1;
    }
    if (pthread_mutex_init(&advisor->mutex, NULL) != 0) {
        curl_global_cleanup();
        return -1;
    }
    if (pthread_cond_init(&advisor->condition, NULL) != 0) {
        pthread_mutex_destroy(&advisor->mutex);
        curl_global_cleanup();
        return -1;
    }
    if (pthread_create(&advisor->thread, NULL, advisor_thread, advisor) != 0) {
        pthread_cond_destroy(&advisor->condition);
        pthread_mutex_destroy(&advisor->mutex);
        curl_global_cleanup();
        return -1;
    }
    advisor->started = true;
    return 0;
}

int wspr_band_advisor_start(struct wspr_band_advisor *advisor,
                            const char *grid,
                            const struct wspr_hop_plan *candidates)
{
    return band_advisor_start(advisor, grid, candidates, false);
}

int ft8_band_advisor_start(struct wspr_band_advisor *advisor,
                           const char *grid,
                           const struct wspr_hop_plan *candidates)
{
    return band_advisor_start(advisor, grid, candidates, true);
}

bool wspr_band_advisor_take(struct wspr_band_advisor *advisor,
                            struct wspr_hop_plan *plan)
{
    bool available;

    if (advisor == NULL || plan == NULL || !advisor->started) {
        return false;
    }
    pthread_mutex_lock(&advisor->mutex);
    available = advisor->update_ready;
    if (available) {
        *plan = advisor->pending;
        advisor->update_ready = false;
    }
    pthread_mutex_unlock(&advisor->mutex);
    return available;
}

bool wspr_band_advisor_wait(struct wspr_band_advisor *advisor,
                            struct wspr_hop_plan *plan,
                            unsigned int timeout_seconds)
{
    struct timespec deadline;
    bool available;

    if (advisor == NULL || plan == NULL || !advisor->started ||
        clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }
    deadline.tv_sec += (time_t)timeout_seconds;
    pthread_mutex_lock(&advisor->mutex);
    while (!advisor->stopping && !advisor->update_ready) {
        int result = pthread_cond_timedwait(&advisor->condition,
                                            &advisor->mutex, &deadline);

        if (result == ETIMEDOUT) {
            break;
        }
        if (result != 0) {
            pthread_mutex_unlock(&advisor->mutex);
            return false;
        }
    }
    available = advisor->update_ready;
    if (available) {
        *plan = advisor->pending;
        advisor->update_ready = false;
    }
    pthread_mutex_unlock(&advisor->mutex);
    return available;
}

void wspr_band_advisor_stop(struct wspr_band_advisor *advisor)
{
    if (advisor == NULL || !advisor->started) {
        return;
    }
    pthread_mutex_lock(&advisor->mutex);
    advisor->stopping = true;
    pthread_cond_signal(&advisor->condition);
    pthread_mutex_unlock(&advisor->mutex);
    pthread_join(advisor->thread, NULL);
    pthread_cond_destroy(&advisor->condition);
    pthread_mutex_destroy(&advisor->mutex);
    advisor->started = false;
    curl_global_cleanup();
}
