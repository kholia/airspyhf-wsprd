#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ft8_decoder.h"

static uint16_t little_u16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t little_u32(const uint8_t *data)
{
    return data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static float *load_fixture(const char *path, size_t *sample_count)
{
    uint8_t header[44];
    FILE *file = fopen(path, "rb");
    float *samples;
    size_t count;

    assert(file != NULL);
    assert(fread(header, sizeof(header), 1U, file) == 1U);
    assert(memcmp(header, "RIFF", 4U) == 0);
    assert(memcmp(header + 8U, "WAVEfmt ", 8U) == 0);
    assert(little_u16(header + 20U) == 1U);
    assert(little_u16(header + 22U) == 1U);
    assert(little_u32(header + 24U) == 12000U);
    assert(little_u16(header + 34U) == 16U);
    assert(memcmp(header + 36U, "data", 4U) == 0);
    count = little_u32(header + 40U) / sizeof(int16_t);
    samples = calloc(count, sizeof(*samples));
    assert(samples != NULL);
    for (size_t index = 0U; index < count; index++) {
        uint8_t raw[2];
        assert(fread(raw, sizeof(raw), 1U, file) == 1U);
        samples[index] = (int16_t)little_u16(raw) / 32768.0f;
    }
    assert(fclose(file) == 0);
    *sample_count = count;
    return samples;
}

int main(void)
{
    struct ft8_decoder decoder;
    struct ft8_decode_result results[FT8_MAX_RESULTS];
    size_t sample_count;
    size_t result_count = 0U;
    float *samples = load_fixture("ft8_lib/test/wav/191111_110130.wav",
                                  &sample_count);
    bool found = false;

    assert(ft8_decoder_init(&decoder));
    assert(ft8_decode_frame(&decoder, samples, sample_count, results,
                            FT8_MAX_RESULTS, &result_count) == 0);
    for (size_t index = 0U; index < result_count; index++) {
        if (strcmp(results[index].message, "CQ R7IW LN35") == 0) {
            assert(results[index].audio_frequency_hz > 1285.0f);
            assert(results[index].audio_frequency_hz < 1297.0f);
            assert(strcmp(results[index].transmitter_call, "R7IW") == 0);
            assert(strcmp(results[index].transmitter_grid, "LN35") == 0);
            found = true;
        }
    }
    assert(found);
    ft8_decoder_free(&decoder);
    free(samples);
    printf("ft8 decoder: found upstream reference message among %zu decodes\n",
           result_count);
    return 0;
}
