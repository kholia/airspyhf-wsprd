#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder_bridge.h"

#define C2_SAMPLE_COUNT 45000U

int main(int argc, char **argv)
{
    static const char *expected_calls[] = {
        "ND6P", "W5BIT", "G8VDQ", "WD4LHT", "NM7J",
        "KI7CI", "DJ6OL", "W3HH", "W3BI"
    };
    struct decoder_options options = {0};
    struct decoder_results results[50] = {0};
    float *interleaved;
    float *i_samples;
    float *q_samples;
    FILE *input;
    char name[14];
    int wspr_type;
    double dial_frequency;
    int32_t result_count = 0;

    assert(argc == 2);
    input = fopen(argv[1], "rb");
    assert(input != NULL);
    assert(fread(name, sizeof(name), 1, input) == 1);
    assert(fread(&wspr_type, sizeof(wspr_type), 1, input) == 1);
    assert(fread(&dial_frequency, sizeof(dial_frequency), 1, input) == 1);
    assert(wspr_type == 2);

    interleaved = malloc(sizeof(*interleaved) * C2_SAMPLE_COUNT * 2U);
    i_samples = malloc(sizeof(*i_samples) * C2_SAMPLE_COUNT);
    q_samples = malloc(sizeof(*q_samples) * C2_SAMPLE_COUNT);
    assert(interleaved != NULL && i_samples != NULL && q_samples != NULL);
    assert(fread(interleaved, sizeof(*interleaved), C2_SAMPLE_COUNT * 2U, input) ==
           C2_SAMPLE_COUNT * 2U);
    fclose(input);

    for (uint32_t index = 0; index < C2_SAMPLE_COUNT; index++) {
        i_samples[index] = interleaved[index * 2U];
        q_samples[index] = -interleaved[index * 2U + 1U];
    }
    free(interleaved);

    options.freq = (uint32_t)(dial_frequency * 1e6);
    options.usehashtable = 0;
    options.npasses = 3;
    options.subtraction = 1;
    snprintf(options.date, sizeof(options.date), "150426");
    snprintf(options.uttime, sizeof(options.uttime), "0918");

    assert(decode_wspr_frame(i_samples, q_samples, C2_SAMPLE_COUNT, &options,
                             results, 50, &result_count) == 0);
    assert(result_count == 9);
    for (int32_t index = 0; index < result_count; index++) {
        assert(strcmp(results[index].call, expected_calls[index]) == 0);
    }

    free(i_samples);
    free(q_samples);
    printf("decoder bridge: parsed all 9 expected spots\n");
    return 0;
}
