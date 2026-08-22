#include <stdint.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ft8_decoder.h"
#include "ft8/decode.h"
#include "ft8/message.h"

#define FT8_MIN_SCORE 5
#define FT8_MAX_CANDIDATES 256
#define FT8_CALLSIGN_TABLE_SIZE 256U

struct callsign_entry {
    char callsign[12];
    uint32_t hash;
};

static struct callsign_entry callsign_table[FT8_CALLSIGN_TABLE_SIZE];

static void callsign_table_reset(void)
{
    memset(callsign_table, 0, sizeof(callsign_table));
}

static void callsign_table_age(void)
{
    for (size_t index = 0U; index < FT8_CALLSIGN_TABLE_SIZE; index++) {
        if (callsign_table[index].callsign[0] == '\0') {
            continue;
        }
        uint8_t age = (uint8_t)(callsign_table[index].hash >> 24);
        if (age > 10U) {
            memset(&callsign_table[index], 0, sizeof(callsign_table[index]));
        } else {
            callsign_table[index].hash =
                ((uint32_t)(age + 1U) << 24) |
                (callsign_table[index].hash & 0x3fffffU);
        }
    }
}

static void save_callsign(const char *callsign, uint32_t hash)
{
    size_t index = ((hash >> 12) & 0x3ffU) * 23U % FT8_CALLSIGN_TABLE_SIZE;

    for (size_t probe = 0U; probe < FT8_CALLSIGN_TABLE_SIZE; probe++) {
        struct callsign_entry *entry = &callsign_table[index];
        if (entry->callsign[0] == '\0') {
            (void)snprintf(entry->callsign, sizeof(entry->callsign), "%.11s",
                           callsign);
            entry->hash = hash & 0x3fffffU;
            return;
        }
        if ((entry->hash & 0x3fffffU) == (hash & 0x3fffffU) &&
            strcmp(entry->callsign, callsign) == 0) {
            entry->hash &= 0x3fffffU;
            return;
        }
        index = (index + 1U) % FT8_CALLSIGN_TABLE_SIZE;
    }
}

static bool lookup_callsign(ftx_callsign_hash_type_t hash_type,
                            uint32_t hash,
                            char *callsign)
{
    uint8_t shift = hash_type == FTX_CALLSIGN_HASH_10_BITS ? 12U :
                    (hash_type == FTX_CALLSIGN_HASH_12_BITS ? 10U : 0U);
    size_t index = ((hash >> (12U - shift)) & 0x3ffU) * 23U %
                   FT8_CALLSIGN_TABLE_SIZE;

    for (size_t probe = 0U; probe < FT8_CALLSIGN_TABLE_SIZE; probe++) {
        const struct callsign_entry *entry = &callsign_table[index];
        if (entry->callsign[0] == '\0') {
            break;
        }
        if (((entry->hash & 0x3fffffU) >> shift) == hash) {
            (void)snprintf(callsign, 12U, "%s", entry->callsign);
            return true;
        }
        index = (index + 1U) % FT8_CALLSIGN_TABLE_SIZE;
    }
    callsign[0] = '\0';
    return false;
}

static ftx_callsign_hash_interface_t callsign_interface = {
    .lookup_hash = lookup_callsign,
    .save_hash = save_callsign
};

static bool normalize_report_callsign(char destination[13], const char *source)
{
    size_t start = 0U;
    size_t length;
    bool has_letter = false;
    bool has_digit = false;

    if (source == NULL) {
        return false;
    }
    length = strlen(source);
    if (length >= 2U && source[0] == '<' && source[length - 1U] == '>') {
        start = 1U;
        length -= 2U;
    }
    if (length < 3U || length > 12U) {
        return false;
    }
    for (size_t index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)source[start + index];
        if (!(isalnum(character) || character == '/')) {
            return false;
        }
        has_letter = has_letter || isalpha(character) != 0;
        has_digit = has_digit || isdigit(character) != 0;
        destination[index] = (char)toupper(character);
    }
    if (!has_letter || !has_digit) {
        destination[0] = '\0';
        return false;
    }
    destination[length] = '\0';
    return true;
}

static bool normalize_report_grid(char destination[7], const char *source)
{
    size_t length;

    if (source == NULL) {
        return false;
    }
    length = strlen(source);
    if (length != 4U && length != 6U) {
        return false;
    }
    for (size_t index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)source[index];
        bool valid = index < 2U ? character >= 'A' && character <= 'R' :
                     (index < 4U ? isdigit(character) != 0 :
                      character >= 'A' && character <= 'X');
        if (!valid) {
            return false;
        }
        destination[index] = index >= 4U ? (char)tolower(character) :
                                          (char)character;
    }
    destination[length] = '\0';
    return true;
}

static void decode_report_fields(const ftx_message_t *message,
                                 struct ft8_decode_result *result)
{
    char call_to[14];
    char call_de[14];
    char extra[14];
    ftx_message_rc_t status;
    ftx_message_type_t type = ftx_message_get_type(message);

    result->transmitter_call[0] = '\0';
    result->transmitter_grid[0] = '\0';
    if (type == FTX_MESSAGE_TYPE_STANDARD) {
        status = ftx_message_decode_std(message, &callsign_interface,
                                        call_to, call_de, extra);
    } else if (type == FTX_MESSAGE_TYPE_NONSTD_CALL) {
        status = ftx_message_decode_nonstd(message, &callsign_interface,
                                           call_to, call_de, extra);
    } else {
        return;
    }
    if (status != FTX_MESSAGE_RC_OK ||
        !normalize_report_callsign(result->transmitter_call, call_de)) {
        result->transmitter_call[0] = '\0';
        return;
    }
    (void)normalize_report_grid(result->transmitter_grid, extra);
}

bool ft8_decoder_init(struct ft8_decoder *decoder)
{
    monitor_config_t configuration = {
        .f_min = 200.0f,
        .f_max = 3000.0f,
        .sample_rate = 12000,
        .time_osr = 4,
        .freq_osr = 2,
        .protocol = FTX_PROTOCOL_FT8,
        .tr_period = 0.0f
    };

    if (decoder == NULL) {
        return false;
    }
    memset(decoder, 0, sizeof(*decoder));
    monitor_init(&decoder->monitor, &configuration);
    decoder->initialized = decoder->monitor.window != NULL &&
                           decoder->monitor.last_frame != NULL &&
                           decoder->monitor.wf.mag != NULL &&
                           decoder->monitor.fft_work != NULL &&
                           decoder->monitor.fft_cfg != NULL;
    if (!decoder->initialized) {
        ft8_decoder_free(decoder);
        return false;
    }
    callsign_table_reset();
    return true;
}

void ft8_decoder_free(struct ft8_decoder *decoder)
{
    if (decoder == NULL) {
        return;
    }
    if (decoder->monitor.window != NULL || decoder->monitor.last_frame != NULL ||
        decoder->monitor.wf.mag != NULL || decoder->monitor.fft_work != NULL) {
        monitor_free(&decoder->monitor);
    }
    memset(decoder, 0, sizeof(*decoder));
}

static bool already_decoded(ftx_message_t *const decoded[],
                            const ftx_message_t *message)
{
    size_t index = message->hash % FT8_MAX_RESULTS;

    for (size_t probe = 0U; probe < FT8_MAX_RESULTS; probe++) {
        if (decoded[index] == NULL) {
            return false;
        }
        if (decoded[index]->hash == message->hash &&
            memcmp(decoded[index]->payload, message->payload,
                   sizeof(message->payload)) == 0) {
            return true;
        }
        index = (index + 1U) % FT8_MAX_RESULTS;
    }
    return true;
}

static bool retain_message(ftx_message_t decoded_storage[],
                           ftx_message_t *decoded[],
                           const ftx_message_t *message)
{
    size_t index = message->hash % FT8_MAX_RESULTS;

    for (size_t probe = 0U; probe < FT8_MAX_RESULTS; probe++) {
        if (decoded[index] == NULL) {
            decoded_storage[index] = *message;
            decoded[index] = &decoded_storage[index];
            return true;
        }
        index = (index + 1U) % FT8_MAX_RESULTS;
    }
    return false;
}

int ft8_decode_frame(struct ft8_decoder *decoder,
                     const float *samples,
                     size_t sample_count,
                     struct ft8_decode_result *results,
                     size_t result_capacity,
                     size_t *result_count)
{
    ftx_message_t decoded_storage[FT8_MAX_RESULTS];
    ftx_message_t *decoded[FT8_MAX_RESULTS] = {0};
    ftx_candidate_t candidates[FT8_MAX_CANDIDATES];
    size_t count = 0U;

    if (decoder == NULL || !decoder->initialized || samples == NULL ||
        results == NULL || result_capacity == 0U || result_count == NULL) {
        return -1;
    }
    *result_count = 0U;
    monitor_reset(&decoder->monitor);
    float frame[1920];
    for (size_t position = 0U; position + 1920U <= sample_count;
         position += 1920U) {
        for (size_t index = 0U; index < 1920U; index++) {
            frame[index] = samples[position + index];
        }
        monitor_process(&decoder->monitor, frame);
    }

    for (unsigned int pass = 0U; pass < 3U && count < result_capacity; pass++) {
        bool frequency_decoded[1024] = {false};
        int candidate_count = ftx_find_candidates(
            &decoder->monitor.wf, FT8_MAX_CANDIDATES / (int)(pass + 1U),
            candidates, FT8_MIN_SCORE);

        for (int index = 0; index < candidate_count && count < result_capacity;
             index++) {
            const ftx_candidate_t *candidate = &candidates[index];
            float frequency_hz =
                (decoder->monitor.min_bin + candidate->freq_offset +
                 (float)candidate->freq_sub / decoder->monitor.wf.freq_osr) /
                decoder->monitor.symbol_period;
            float time_seconds =
                (candidate->time_offset +
                 (float)candidate->time_sub / decoder->monitor.wf.time_osr) *
                decoder->monitor.symbol_period;
            int frequency_bucket = (int)frequency_hz / 4;
            ftx_message_t message;
            ftx_decode_status_t status;

            if (frequency_bucket < 0 || frequency_bucket >= 1024 ||
                frequency_decoded[frequency_bucket]) {
                continue;
            }
            if (!ftx_decode_candidate(&decoder->monitor.wf, candidate,
                                      decoder->monitor.wf.desc->max_ldpc_iterations,
                                      &message, &status)) {
                continue;
            }
            frequency_decoded[frequency_bucket] = true;
            if (already_decoded(decoded, &message)) {
                continue;
            }
            char text[FTX_MAX_MESSAGE_LENGTH];
            if (ftx_message_decode(&message, &callsign_interface, text) !=
                    FTX_MESSAGE_RC_OK ||
                !retain_message(decoded_storage, decoded, &message)) {
                continue;
            }
            results[count].snr = message.snr / 2.0f - 22.0f;
            results[count].dt = time_seconds;
            results[count].audio_frequency_hz = frequency_hz;
            decode_report_fields(&message, &results[count]);
            (void)snprintf(results[count].message,
                           sizeof(results[count].message), "%s", text);
            count++;
        }
    }
    callsign_table_age();
    *result_count = count;
    return 0;
}
