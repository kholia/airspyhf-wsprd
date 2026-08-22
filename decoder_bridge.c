#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "decoder_bridge.h"

#define C2_SAMPLE_COUNT 45000U

static char process_runtime_directory[PATH_MAX];
static bool process_runtime_owned;

static char *join_path(const char *directory, const char *name)
{
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    size_t position;
    char *path;

    if (name_length > SIZE_MAX - 2U ||
        directory_length > SIZE_MAX - name_length - 2U) {
        return NULL;
    }
    path = malloc(directory_length + name_length + 2U);
    if (path == NULL) {
        return NULL;
    }

    memcpy(path, directory, directory_length);
    position = directory_length;
    if (position != 0 && path[position - 1U] != '/') {
        path[position++] = '/';
    }
    memcpy(path + position, name, name_length + 1U);
    return path;
}

static void remove_runtime_file(const char *directory, const char *name)
{
    char *path = join_path(directory, name);

    if (path != NULL) {
        (void)unlink(path);
        free(path);
    }
}

static void cleanup_process_runtime(void)
{
    if (!process_runtime_owned) {
        return;
    }
    remove_runtime_file(process_runtime_directory, "ALL_WSPR.TXT");
    remove_runtime_file(process_runtime_directory, "wspr_spots.txt");
    remove_runtime_file(process_runtime_directory, "wspr_timer.out");
    remove_runtime_file(process_runtime_directory, "hashtable.txt");
    (void)rmdir(process_runtime_directory);
}

static const char *get_runtime_directory(void)
{
    const char *configured = getenv("AIRSPYHF_WSPRD_RUNTIME_DIR");
    const char *base;

    if (configured != NULL && configured[0] != '\0') {
        return configured;
    }
    if (process_runtime_directory[0] != '\0') {
        return process_runtime_directory;
    }
#ifdef __linux__
    base = "/dev/shm";
#else
    base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
#endif
    if (snprintf(process_runtime_directory, sizeof(process_runtime_directory),
                 "%s/airspyhf-wsprd-%ld-XXXXXX", base, (long)getpid()) >=
        (int)sizeof(process_runtime_directory) ||
        mkdtemp(process_runtime_directory) == NULL) {
        process_runtime_directory[0] = '\0';
        return NULL;
    }
    process_runtime_owned = true;
    (void)atexit(cleanup_process_runtime);
    return process_runtime_directory;
}

static const char *core_executable(void)
{
    const char *configured = getenv("AIRSPYHF_WSPRD_CORE");
    if (configured != NULL && configured[0] != '\0') {
        return configured;
    }
    return access("./airspyhf-wsprd-core", X_OK) == 0 ?
           "./airspyhf-wsprd-core" : "airspyhf-wsprd-core";
}

static int write_c2(const char *path,
                    const float *i_samples,
                    const float *q_samples,
                    uint32_t sample_count,
                    double dial_frequency_mhz)
{
    FILE *output;
    char header_name[14] = {0};
    int wspr_type = 2;
    float *interleaved;
    uint32_t index;
    int status = -1;

    output = fopen(path, "wb");
    if (output == NULL) {
        fprintf(stderr, "Unable to create decoder input %s: %s\n", path, strerror(errno));
        return -1;
    }

    const char *base_name = strrchr(path, '/');
    base_name = base_name == NULL ? path : base_name + 1;
    memcpy(header_name, base_name, strlen(base_name) < sizeof(header_name) ?
           strlen(base_name) : sizeof(header_name));

    interleaved = calloc(C2_SAMPLE_COUNT * 2U, sizeof(*interleaved));
    if (interleaved == NULL) {
        fprintf(stderr, "Unable to allocate C2 decoder buffer\n");
        fclose(output);
        return -1;
    }

    if (sample_count > C2_SAMPLE_COUNT) {
        sample_count = C2_SAMPLE_COUNT;
    }
    for (index = 0; index < sample_count; index++) {
        interleaved[index * 2U] = i_samples[index];
        /* wsprd's C2 reader restores Q by negating this stored component. */
        interleaved[index * 2U + 1U] = -q_samples[index];
    }

    if (fwrite(header_name, sizeof(header_name), 1, output) == 1 &&
        fwrite(&wspr_type, sizeof(wspr_type), 1, output) == 1 &&
        fwrite(&dial_frequency_mhz, sizeof(dial_frequency_mhz), 1, output) == 1 &&
        fwrite(interleaved, sizeof(*interleaved), C2_SAMPLE_COUNT * 2U, output) ==
            C2_SAMPLE_COUNT * 2U) {
        status = 0;
    } else {
        fprintf(stderr, "Unable to write complete decoder input %s\n", path);
    }

    free(interleaved);
    if (fclose(output) != 0) {
        status = -1;
    }
    return status;
}

static int run_core(const char *input_path,
                    const char *data_directory,
                    const struct decoder_options *options)
{
    const char *arguments[12];
    size_t argument_count = 0;
    pid_t child;
    pid_t waited;
    int child_status;

    arguments[argument_count++] = core_executable();
    arguments[argument_count++] = "-a";
    arguments[argument_count++] = data_directory;
    if (!options->usehashtable) {
        arguments[argument_count++] = "-H";
    }
    if (options->quickmode) {
        arguments[argument_count++] = "-q";
    }
    if (!options->subtraction || options->npasses == 1) {
        arguments[argument_count++] = "-s";
    }
    arguments[argument_count++] = input_path;
    arguments[argument_count] = NULL;

    child = fork();
    if (child < 0) {
        fprintf(stderr, "Unable to start wsprd core: %s\n", strerror(errno));
        return -1;
    }
    if (child == 0) {
        int null_output = open("/dev/null", O_WRONLY);
        if (null_output >= 0) {
            dup2(null_output, STDOUT_FILENO);
            close(null_output);
        }
        execvp(arguments[0], (char *const *)arguments);
        fprintf(stderr, "Unable to execute %s: %s\n", arguments[0], strerror(errno));
        _exit(127);
    }

    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) {
        fprintf(stderr, "Unable to wait for wsprd core: %s\n", strerror(errno));
        return -1;
    }

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "wsprd core failed with status %d\n",
                WIFEXITED(child_status) ? WEXITSTATUS(child_status) : -1);
        return -1;
    }
    return 0;
}

static int parse_results(struct decoder_results *results,
                         uint32_t result_capacity,
                         int32_t *result_count,
                         const char *data_directory)
{
    char *spots_path;
    FILE *input;
    char line[256];
    uint32_t count = 0;

    spots_path = join_path(data_directory, "wspr_spots.txt");
    if (spots_path == NULL) {
        fprintf(stderr, "Unable to allocate decoder results path\n");
        return -1;
    }
    input = fopen(spots_path, "r");
    free(spots_path);

    *result_count = 0;
    if (input == NULL) {
        fprintf(stderr, "wsprd core did not create wspr_spots.txt\n");
        return -1;
    }

    while (count < result_capacity && fgets(line, sizeof(line), input) != NULL) {
        char date[7];
        char time[5];
        char call[13];
        char grid[7];
        char power[3];
        int sync_tenths;
        int drift;
        int jitter;
        unsigned int cycles;
        float snr;
        float dt;
        double frequency;

        int fields = sscanf(line,
                            "%6s %4s %d %f %f %lf %12s %6s %2s %d %u %d",
                            date, time, &sync_tenths, &snr, &dt, &frequency,
                            call, grid, power, &drift, &cycles, &jitter);
        if (fields != 12) {
            fprintf(stderr, "Unable to parse wsprd result: %s", line);
            fclose(input);
            return -1;
        }

        results[count].freq = frequency;
        results[count].sync = sync_tenths / 10.0f;
        results[count].snr = snr;
        results[count].dt = dt;
        results[count].drift = drift;
        results[count].jitter = jitter;
        results[count].cycles = cycles;
        snprintf(results[count].call, sizeof(results[count].call), "%s", call);
        snprintf(results[count].loc, sizeof(results[count].loc), "%s", grid);
        snprintf(results[count].pwr, sizeof(results[count].pwr), "%s", power);
        snprintf(results[count].message, sizeof(results[count].message),
                 "%s %s %s", call, grid, power);
        count++;
    }

    fclose(input);
    *result_count = (int32_t)count;
    return 0;
}

int decode_wspr_frame(const float *i_samples,
                      const float *q_samples,
                      uint32_t sample_count,
                      const struct decoder_options *options,
                      struct decoder_results *results,
                      uint32_t result_capacity,
                      int32_t *result_count)
{
    const char *runtime_directory = get_runtime_directory();
    char temporary_directory[PATH_MAX];
    char input_name[15];
    char *input_path;
    int status = -1;

    if (i_samples == NULL || q_samples == NULL || options == NULL || results == NULL ||
        result_count == NULL || result_capacity == 0) {
        return -1;
    }
    *result_count = 0;

    if (runtime_directory == NULL) {
        fprintf(stderr, "Unable to create RAM decoder runtime directory\n");
        return -1;
    }
    if (snprintf(temporary_directory, sizeof(temporary_directory),
                 "%s/airspyhf-wsprd-XXXXXX", runtime_directory) >=
        (int)sizeof(temporary_directory)) {
        fprintf(stderr, "Decoder runtime path is too long\n");
        return -1;
    }
    if (mkdtemp(temporary_directory) == NULL) {
        fprintf(stderr, "Unable to create decoder temporary directory: %s\n", strerror(errno));
        return -1;
    }
    if (snprintf(input_name, sizeof(input_name), "%.6s_%.4s.c2",
                 options->date, options->uttime) != 14) {
        fprintf(stderr, "Unable to construct decoder input name\n");
        (void)rmdir(temporary_directory);
        return -1;
    }
    input_path = join_path(temporary_directory, input_name);
    if (input_path == NULL) {
        fprintf(stderr, "Unable to allocate decoder input path\n");
        (void)rmdir(temporary_directory);
        return -1;
    }

    if (write_c2(input_path, i_samples, q_samples, sample_count,
                 options->freq / 1e6) == 0 &&
        run_core(input_path, runtime_directory, options) == 0) {
        status = parse_results(results, result_capacity, result_count,
                               runtime_directory);
    }

    static const char *const generated_files[] = {
        "ALL_WSPR.TXT", "wspr_spots.txt", "wspr_timer.out"
    };
    if (unlink(input_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "Unable to remove temporary decoder input: %s\n", strerror(errno));
    }
    for (size_t i = 0; i < sizeof(generated_files) / sizeof(generated_files[0]); i++) {
        char *generated_path = join_path(runtime_directory, generated_files[i]);
        if (generated_path == NULL) {
            fprintf(stderr, "Unable to allocate decoder cleanup path\n");
        } else if (unlink(generated_path) != 0 && errno != ENOENT) {
            fprintf(stderr, "Unable to remove decoder runtime file: %s\n", strerror(errno));
        }
        free(generated_path);
    }
    free(input_path);
    if (rmdir(temporary_directory) != 0) {
        fprintf(stderr, "Unable to remove decoder temporary directory: %s\n", strerror(errno));
    }
    return status;
}
