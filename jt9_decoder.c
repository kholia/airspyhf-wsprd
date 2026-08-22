#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jt9_decoder.h"
#include "station.h"

#define JT9_WAVE_SAMPLES 180000U

extern char **environ;

static char *join_path(const char *directory, const char *name)
{
    size_t directory_length;
    size_t name_length;
    char *path;

    if (directory == NULL || name == NULL) {
        return NULL;
    }
    directory_length = strlen(directory);
    name_length = strlen(name);
    if (directory_length > SIZE_MAX - name_length - 2U) {
        return NULL;
    }
    path = malloc(directory_length + name_length + 2U);
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, directory, directory_length);
    path[directory_length] = '/';
    memcpy(path + directory_length + 1U, name, name_length + 1U);
    return path;
}

static bool write_all(int descriptor, const void *buffer, size_t length)
{
    const uint8_t *position = buffer;

    while (length != 0U) {
        ssize_t written = write(descriptor, position, length);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        position += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

static void store_u16le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void store_u32le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static bool write_wave(const char *path,
                       const float *samples,
                       size_t sample_count)
{
    uint8_t header[44] = {0};
    int16_t pcm[2048];
    float peak = 1e-12f;
    float scale;
    size_t position = 0U;
    int descriptor;

    if (path == NULL || samples == NULL) {
        return false;
    }
    for (size_t index = 0U; index < sample_count; index++) {
        if (isfinite(samples[index])) {
            peak = fmaxf(peak, fabsf(samples[index]));
        }
    }
    scale = 29490.0f / peak;

    memcpy(header, "RIFF", 4U);
    store_u32le(header + 4U, 36U + JT9_WAVE_SAMPLES * 2U);
    memcpy(header + 8U, "WAVEfmt ", 8U);
    store_u32le(header + 16U, 16U);
    store_u16le(header + 20U, 1U);
    store_u16le(header + 22U, 1U);
    store_u32le(header + 24U, 12000U);
    store_u32le(header + 28U, 24000U);
    store_u16le(header + 32U, 2U);
    store_u16le(header + 34U, 16U);
    memcpy(header + 36U, "data", 4U);
    store_u32le(header + 40U, JT9_WAVE_SAMPLES * 2U);

    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0 || !write_all(descriptor, header, sizeof(header))) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return false;
    }

    while (position < JT9_WAVE_SAMPLES) {
        size_t chunk = JT9_WAVE_SAMPLES - position;
        if (chunk > sizeof(pcm) / sizeof(pcm[0])) {
            chunk = sizeof(pcm) / sizeof(pcm[0]);
        }
        for (size_t index = 0U; index < chunk; index++) {
            size_t source_index = position + index;
            float value = source_index < sample_count &&
                          isfinite(samples[source_index]) ?
                          samples[source_index] * scale : 0.0f;
            value = fmaxf(-32767.0f, fminf(32767.0f, value));
            pcm[index] = (int16_t)lrintf(value);
        }
        if (!write_all(descriptor, pcm, chunk * sizeof(pcm[0]))) {
            (void)close(descriptor);
            return false;
        }
        position += chunk;
    }
    return close(descriptor) == 0;
}

static void remove_runtime_file(const struct jt9_decoder *decoder,
                                const char *name)
{
    char *path = join_path(decoder->runtime_directory, name);
    if (path != NULL) {
        (void)unlink(path);
        free(path);
    }
}

bool jt9_decoder_init(struct jt9_decoder *decoder,
                      const char *executable,
                      const char *callsign,
                      const char *grid)
{
    const char *base;
    size_t template_length;
    char *directory_template;

    if (decoder == NULL) {
        return false;
    }
    memset(decoder, 0, sizeof(*decoder));
    if (executable == NULL || executable[0] == '\0' ||
        access(executable, X_OK) != 0) {
        return false;
    }
    base = getenv("AIRSPYHF_WSPRD_RUNTIME_DIR");
    if (base == NULL || base[0] == '\0') {
        base = access("/dev/shm", W_OK) == 0 ? "/dev/shm" : "/tmp";
    }
    template_length = strlen(base) + sizeof("/jt9.XXXXXX");
    directory_template = malloc(template_length);
    if (directory_template == NULL) {
        return false;
    }
    (void)snprintf(directory_template, template_length, "%s/jt9.XXXXXX", base);
    if (mkdtemp(directory_template) == NULL) {
        free(directory_template);
        return false;
    }
    decoder->executable = strdup(executable);
    decoder->runtime_directory = directory_template;
    if (decoder->executable == NULL) {
        (void)rmdir(directory_template);
        free(directory_template);
        memset(decoder, 0, sizeof(*decoder));
        return false;
    }
    (void)snprintf(decoder->callsign, sizeof(decoder->callsign), "%s",
                   callsign == NULL ? "" : callsign);
    (void)snprintf(decoder->grid, sizeof(decoder->grid), "%s",
                   grid == NULL ? "" : grid);
    decoder->enabled = true;
    return true;
}

void jt9_decoder_free(struct jt9_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
    if (decoder->runtime_directory != NULL) {
        static const char *const files[] = {
            "decoded.txt", "houndcallers.txt", "jt9_wisdom.dat",
            "rx_messages.txt", "timer.out"
        };
        for (size_t index = 0U; index < sizeof(files) / sizeof(files[0]);
             index++) {
            remove_runtime_file(decoder, files[index]);
        }
        (void)rmdir(decoder->runtime_directory);
    }
    free(decoder->runtime_directory);
    free(decoder->executable);
    memset(decoder, 0, sizeof(*decoder));
}

int jt9_decoder_start(struct jt9_decoder *decoder,
                      const float *samples,
                      size_t sample_count,
                      time_t frame_start_unix,
                      struct jt9_decode_job *job)
{
    char wave_name[26];
    struct tm frame_time;
    posix_spawn_file_actions_t actions;
    int output_descriptor = -1;
    int spawn_status;

    if (job == NULL) {
        return -1;
    }
    memset(job, 0, sizeof(*job));
    if (decoder != NULL) {
        decoder->last_error[0] = '\0';
    }
    if (decoder == NULL || !decoder->enabled || samples == NULL ||
        gmtime_r(&frame_start_unix, &frame_time) == NULL ||
        strftime(wave_name, sizeof(wave_name), "%Y%m%d_%H%M%S.wav",
                 &frame_time) != 19U) {
        return -1;
    }
    job->wave_path = join_path(decoder->runtime_directory, wave_name);
    job->output_path = join_path(decoder->runtime_directory, "decode.out");
    if (job->wave_path == NULL || job->output_path == NULL ||
        !write_wave(job->wave_path, samples, sample_count)) {
        goto failure;
    }
    output_descriptor = open(job->output_path,
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (output_descriptor < 0 || posix_spawn_file_actions_init(&actions) != 0) {
        goto failure;
    }
    if (posix_spawn_file_actions_adddup2(&actions, output_descriptor,
                                         STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, output_descriptor,
                                         STDERR_FILENO) != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        goto failure;
    }

    char *arguments[] = {
        decoder->executable,
        "--ft8", "-M", "-C", "2", "-m", "2", "-q",
        "-a", decoder->runtime_directory,
        "-t", decoder->runtime_directory,
        "-e", decoder->runtime_directory,
        "-c", decoder->callsign,
        "-G", decoder->grid,
        job->wave_path,
        NULL
    };
    spawn_status = posix_spawn(&job->child, decoder->executable, &actions,
                               NULL, arguments, environ);
    (void)posix_spawn_file_actions_destroy(&actions);
    (void)close(output_descriptor);
    output_descriptor = -1;
    if (spawn_status != 0) {
        errno = spawn_status;
        goto failure;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &job->started_at);
    job->timeout_seconds = JT9_DECODE_TIMEOUT_SECONDS;
    job->active = true;
    return 0;

failure:
    if (decoder != NULL && decoder->last_error[0] == '\0') {
        (void)snprintf(decoder->last_error, sizeof(decoder->last_error),
                       "could not create the RAM input or child process");
    }
    if (output_descriptor >= 0) {
        (void)close(output_descriptor);
    }
    if (job->wave_path != NULL) {
        (void)unlink(job->wave_path);
    }
    if (job->output_path != NULL) {
        (void)unlink(job->output_path);
    }
    free(job->wave_path);
    free(job->output_path);
    memset(job, 0, sizeof(*job));
    return -1;
}

static bool normalize_jt9_callsign(char destination[13], const char *source)
{
    char unwrapped[13];
    size_t length;

    if (source == NULL) {
        return false;
    }
    length = strlen(source);
    if (length >= 2U && source[0] == '<' && source[length - 1U] == '>' &&
        length - 2U < sizeof(unwrapped)) {
        memcpy(unwrapped, source + 1U, length - 2U);
        unwrapped[length - 2U] = '\0';
        source = unwrapped;
    }
    return station_normalize_callsign(destination, 13U, source);
}

static void decode_report_fields(struct ft8_decode_result *result)
{
    char message[sizeof(result->message)];
    char *tokens[8];
    size_t token_count = 0U;
    char *save = NULL;
    char *token;
    size_t sender_index = SIZE_MAX;

    (void)snprintf(message, sizeof(message), "%s", result->message);
    token = strtok_r(message, " ", &save);
    while (token != NULL && token_count < sizeof(tokens) / sizeof(tokens[0])) {
        tokens[token_count++] = token;
        token = strtok_r(NULL, " ", &save);
    }
    result->transmitter_call[0] = '\0';
    result->transmitter_grid[0] = '\0';
    if (token_count < 2U) {
        return;
    }
    if (strcmp(tokens[0], "CQ") == 0) {
        for (size_t index = 1U; index < token_count; index++) {
            if (normalize_jt9_callsign(result->transmitter_call,
                                       tokens[index])) {
                sender_index = index;
                break;
            }
        }
    } else if (normalize_jt9_callsign(result->transmitter_call, tokens[1])) {
        sender_index = 1U;
    }
    if (sender_index != SIZE_MAX && sender_index + 1U < token_count) {
        (void)station_normalize_grid(result->transmitter_grid,
                                     sizeof(result->transmitter_grid),
                                     tokens[sender_index + 1U]);
    }
}

bool jt9_parse_decode_line(const char *line,
                           struct ft8_decode_result *result)
{
    char utc[7];
    char marker[8];
    int message_offset = 0;
    int matched;
    size_t message_length;

    if (line == NULL || result == NULL) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    matched = sscanf(line, "%6[0-9] %f %f %f %7s %n", utc, &result->snr,
                     &result->dt, &result->audio_frequency_hz, marker,
                     &message_offset);
    if (matched != 5 || strlen(utc) != 6U || strcmp(marker, "~") != 0 ||
        message_offset <= 0 || result->snr < -60.0f || result->snr > 60.0f ||
        result->dt < -5.0f || result->dt > 5.0f ||
        result->audio_frequency_hz < 100.0f ||
        result->audio_frequency_hz > 5000.0f) {
        return false;
    }
    (void)snprintf(result->message, sizeof(result->message), "%s",
                   line + message_offset);
    message_length = strlen(result->message);
    while (message_length != 0U &&
           isspace((unsigned char)result->message[message_length - 1U])) {
        result->message[--message_length] = '\0';
    }
    if (message_length == 0U) {
        return false;
    }
    decode_report_fields(result);
    return true;
}

static int wait_for_child(struct jt9_decoder *decoder,
                          struct jt9_decode_job *job)
{
    struct timespec now;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000L};
    int status = 0;

    for (;;) {
        pid_t waited = waitpid(job->child, &status, WNOHANG);
        if (waited == job->child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                return 0;
            }
            if (WIFEXITED(status)) {
                (void)snprintf(decoder->last_error,
                               sizeof(decoder->last_error),
                               "exited with status %d", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                (void)snprintf(decoder->last_error,
                               sizeof(decoder->last_error),
                               "terminated by signal %d", WTERMSIG(status));
            } else {
                (void)snprintf(decoder->last_error,
                               sizeof(decoder->last_error),
                               "ended unexpectedly");
            }
            return -1;
        }
        if (waited < 0 && errno != EINTR) {
            (void)snprintf(decoder->last_error, sizeof(decoder->last_error),
                           "waitpid failed: %s", strerror(errno));
            return -1;
        }
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        time_t elapsed_seconds = now.tv_sec - job->started_at.tv_sec;
        if (elapsed_seconds >= (time_t)job->timeout_seconds) {
            (void)kill(job->child, SIGKILL);
            while (waitpid(job->child, &status, 0) < 0 && errno == EINTR) {
            }
            (void)snprintf(decoder->last_error, sizeof(decoder->last_error),
                           "timed out after %u seconds",
                           job->timeout_seconds);
            return -1;
        }
        (void)nanosleep(&pause, NULL);
    }
}

static bool failure_diagnostic(const char *line)
{
    return strstr(line, "error") != NULL || strstr(line, "Error") != NULL ||
           strstr(line, "failed") != NULL || strstr(line, "Permission denied") != NULL;
}

int jt9_decoder_finish(struct jt9_decoder *decoder,
                       struct jt9_decode_job *job,
                       struct ft8_decode_result *results,
                       size_t result_capacity,
                       size_t *result_count)
{
    FILE *output = NULL;
    char *line = NULL;
    size_t line_capacity = 0U;
    size_t count = 0U;
    int child_status;
    int status = -1;

    if (decoder == NULL || job == NULL || !job->active || results == NULL ||
        result_capacity == 0U || result_count == NULL) {
        return -1;
    }
    *result_count = 0U;
    decoder->last_error[0] = '\0';
    child_status = wait_for_child(decoder, job);
    output = fopen(job->output_path, "r");
    if (output == NULL) {
        goto cleanup;
    }
    if (child_status != 0) {
        while (getline(&line, &line_capacity, output) >= 0) {
            if (failure_diagnostic(line)) {
                size_t length = strcspn(line, "\r\n");
                line[length] = '\0';
                (void)snprintf(decoder->last_error,
                               sizeof(decoder->last_error), "%s", line);
                break;
            }
        }
        goto cleanup;
    }
    while (count < result_capacity &&
           getline(&line, &line_capacity, output) >= 0) {
        if (jt9_parse_decode_line(line, &results[count])) {
            count++;
        }
    }
    *result_count = count;
    status = 0;

cleanup:
    free(line);
    if (output != NULL) {
        (void)fclose(output);
    }
    (void)unlink(job->wave_path);
    (void)unlink(job->output_path);
    free(job->wave_path);
    free(job->output_path);
    memset(job, 0, sizeof(*job));
    remove_runtime_file(decoder, "decoded.txt");
    remove_runtime_file(decoder, "houndcallers.txt");
    remove_runtime_file(decoder, "rx_messages.txt");
    remove_runtime_file(decoder, "timer.out");
    return status;
}

bool jt9_decoder_warmup(struct jt9_decoder *decoder)
{
    static const float silence[JT9_WAVE_SAMPLES] = {0};
    struct jt9_decode_job job;
    struct ft8_decode_result ignored_result;
    size_t ignored_count = 0U;

    if (jt9_decoder_start(decoder, silence, JT9_WAVE_SAMPLES, 0, &job) != 0) {
        return false;
    }
    job.timeout_seconds = JT9_WARMUP_TIMEOUT_SECONDS;
    return jt9_decoder_finish(decoder, &job, &ignored_result, 1U,
                              &ignored_count) == 0;
}

size_t jt9_merge_results(struct ft8_decode_result *primary,
                         size_t primary_count,
                         size_t capacity,
                         const struct ft8_decode_result *additional,
                         size_t additional_count)
{
    if (primary == NULL || additional == NULL || primary_count > capacity) {
        return primary_count;
    }
    for (size_t addition = 0U;
         addition < additional_count && primary_count < capacity; addition++) {
        bool duplicate = false;
        for (size_t existing = 0U; existing < primary_count; existing++) {
            if (strcmp(primary[existing].message,
                       additional[addition].message) == 0 &&
                fabsf(primary[existing].audio_frequency_hz -
                      additional[addition].audio_frequency_hz) <= 10.0f) {
                if (primary[existing].transmitter_call[0] == '\0' &&
                    additional[addition].transmitter_call[0] != '\0') {
                    (void)snprintf(primary[existing].transmitter_call,
                                   sizeof(primary[existing].transmitter_call),
                                   "%s", additional[addition].transmitter_call);
                    (void)snprintf(primary[existing].transmitter_grid,
                                   sizeof(primary[existing].transmitter_grid),
                                   "%s", additional[addition].transmitter_grid);
                }
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            primary[primary_count++] = additional[addition];
        }
    }
    return primary_count;
}
