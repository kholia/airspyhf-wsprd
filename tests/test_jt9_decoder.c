#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jt9_decoder.h"

static void test_parse(void)
{
    struct ft8_decode_result result;

    assert(jt9_parse_decode_line(
        "000000   0  0.8 1369 ~  CQ OK6LZ JN99                 \n",
        &result));
    assert(result.snr == 0.0f);
    assert(result.dt == 0.8f);
    assert(result.audio_frequency_hz == 1369.0f);
    assert(strcmp(result.message, "CQ OK6LZ JN99") == 0);
    assert(strcmp(result.transmitter_call, "OK6LZ") == 0);
    assert(strcmp(result.transmitter_grid, "JN99") == 0);

    assert(jt9_parse_decode_line(
        "110130  -7  0.9 2096 ~  CQ DX R6WA LN32\n", &result));
    assert(strcmp(result.transmitter_call, "R6WA") == 0);
    assert(strcmp(result.transmitter_grid, "LN32") == 0);

    assert(jt9_parse_decode_line(
        "110130 -16  1.0  990 ~  OH3NIV ZS6S -03\n", &result));
    assert(strcmp(result.transmitter_call, "ZS6S") == 0);
    assert(result.transmitter_grid[0] == '\0');

    assert(!jt9_parse_decode_line("<DecodeFinished>   0  30  0\n", &result));
    assert(!jt9_parse_decode_line(
        "110130 -16  1.0  990 +  OH3NIV ZS6S -03\n", &result));
}

static void test_merge(void)
{
    struct ft8_decode_result primary[4] = {0};
    struct ft8_decode_result additional[2] = {0};

    primary[0].audio_frequency_hz = 1000.0f;
    (void)snprintf(primary[0].message, sizeof(primary[0].message),
                   "CQ VU3CER MK68");

    additional[0].audio_frequency_hz = 1007.0f;
    (void)snprintf(additional[0].message, sizeof(additional[0].message),
                   "CQ VU3CER MK68");
    (void)snprintf(additional[0].transmitter_call,
                   sizeof(additional[0].transmitter_call), "VU3CER");
    (void)snprintf(additional[0].transmitter_grid,
                   sizeof(additional[0].transmitter_grid), "MK68");

    additional[1] = additional[0];
    additional[1].audio_frequency_hz = 1020.0f;

    size_t count = jt9_merge_results(primary, 1U, 4U, additional, 2U);
    assert(count == 2U);
    assert(strcmp(primary[0].transmitter_call, "VU3CER") == 0);
    assert(primary[1].audio_frequency_hz == 1020.0f);
}

static void test_ram_process_bridge(void)
{
    static float samples[175200];
    struct jt9_decoder decoder;
    struct jt9_decode_job job;
    struct ft8_decode_result results[4];
    size_t result_count = 0U;
    char base_template[] = "/tmp/airspyhf-jt9-test.XXXXXX";
    char executable[256];
    char *base = mkdtemp(base_template);
    FILE *script;

    assert(base != NULL);
    assert(snprintf(executable, sizeof(executable), "%s/fake-jt9", base) > 0);
    script = fopen(executable, "w");
    assert(script != NULL);
    assert(fputs("#!/bin/sh\n"
                 "printf '%s\\n' '110130 -12 0.9 1500 ~  CQ K1TE FN42'\n",
                 script) >= 0);
    assert(fclose(script) == 0);
    assert(chmod(executable, 0755) == 0);
    assert(setenv("AIRSPYHF_WSPRD_RUNTIME_DIR", base, 1) == 0);

    assert(jt9_decoder_init(&decoder, executable, "VU3CER", "MK68xm"));
    assert(jt9_decoder_warmup(&decoder));
    assert(jt9_decoder_start(&decoder, samples,
                             sizeof(samples) / sizeof(samples[0]),
                             1787392800, &job) == 0);
    assert(jt9_decoder_finish(&decoder, &job, results, 4U,
                              &result_count) == 0);
    assert(result_count == 1U);
    assert(strcmp(results[0].transmitter_call, "K1TE") == 0);
    assert(strcmp(results[0].transmitter_grid, "FN42") == 0);
    jt9_decoder_free(&decoder);

    assert(unsetenv("AIRSPYHF_WSPRD_RUNTIME_DIR") == 0);
    assert(unlink(executable) == 0);
    assert(rmdir(base) == 0);
}

int main(void)
{
    test_parse();
    test_merge();
    test_ram_process_bridge();
    puts("jt9 bridge: RAM process, output parsing, and result deduplication verified");
    return 0;
}
