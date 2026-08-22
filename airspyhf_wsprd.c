/*
 * FreeBSD License
 * Copyright (c) 2016, Guenael
 * All rights reserved.
 *
 * This file is based on AirSpy project & HackRF project
 *   Copyright 2012 Jared Boone <jared@sharebrained.com>
 *   Copyright 2014-2015 Benjamin Vernoux <bvernoux@airspy.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <arpa/inet.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>

#include <airspyhf.h>

#include "airspyhf_wsprd.h"
#include "band_advisor.h"
#include "bands.h"
#include "clock_health.h"
#include "decimator.h"
#include "decoder_bridge.h"
#include "ft8_decoder.h"
#include "ft8_resampler.h"
#include "health_server.h"
#include "jt9_decoder.h"
#include "psk_reporter.h"
#include "reporter.h"
#include "sample_queue.h"
#include "service_notify.h"
#include "station.h"


/* TODO
 - multispot report in one post
 - type fix (uint32_t etc..)
 - verbose option
*/


#define WSPR_CAPTURE_SAMPLES (116U * WSPR_OUTPUT_RATE)
#define FT8_CAPTURE_SAMPLES  175200U
#define MAX_CAPTURE_SAMPLES  FT8_CAPTURE_SAMPLES
#define DSP_QUEUE_SLOTS    32
#define FT8_BOOT_ADVICE_TIMEOUT_SECONDS 45U

enum receiver_mode {
    RECEIVER_MODE_WSPR,
    RECEIVER_MODE_FT8
};


/* Global declaration for these structs */
struct receiver_state   rx_state;
struct receiver_options rx_options;
struct decoder_options  dec_options;
struct decoder_results  dec_results[50];
airspyhf_device_t*      device = NULL;
airspyhf_read_partid_serialno_t readSerial;
struct wspr_decimator  decimator;
struct ft8_resampler   ft8_resampler;
struct ft8_decoder     ft8_decoder;
struct jt9_decoder     jt9_decoder;
struct wspr_reporter   reporter;
struct psk_reporter    psk_reporter;
struct wspr_hop_plan   hop_plan;
struct wspr_hop_plan   advised_hop_plan;
struct wspr_band_advisor band_advisor;
struct wspr_health_server health_server;
const char wsprnet_app_version[] = "airhf-090";
static enum receiver_mode receiver_mode = RECEIVER_MODE_WSPR;
static bool reporting_started;
static bool jt9_enabled;
static unsigned int jt9_consecutive_failures;

static const char automatic_hop_bands[] =
    "80m,40m,30m,20m,17m,15m,12m,10m";

static uint32_t capture_sample_target(void)
{
    return receiver_mode == RECEIVER_MODE_FT8 ? FT8_CAPTURE_SAMPLES :
                                                WSPR_CAPTURE_SAMPLES;
}

static size_t health_report_queue_depth(void *context)
{
    (void)context;
    if (rx_options.noreport) {
        return 0U;
    }
    return receiver_mode == RECEIVER_MODE_FT8 ?
           psk_reporter_pending(&psk_reporter) :
           wspr_reporter_pending(&reporter);
}

static int start_reporting(void)
{
    int result;

    if (rx_options.noreport) {
        return 0;
    }
    if (receiver_mode == RECEIVER_MODE_FT8) {
        result = psk_reporter_start(&psk_reporter, dec_options.rcall,
                                    dec_options.rloc, "airspyhf-wsprd 0.9");
    } else {
        result = wspr_reporter_start(&reporter, wsprnet_app_version);
    }
    reporting_started = result == 0;
    return result;
}

static void stop_reporting(bool warn_pending)
{
    size_t pending;

    if (!reporting_started) {
        return;
    }
    if (receiver_mode == RECEIVER_MODE_FT8) {
        pending = psk_reporter_pending(&psk_reporter);
        if (warn_pending && pending != 0U) {
            fprintf(stderr, "Stopping with %zu unsent PSK Reporter spots in RAM\n",
                    pending);
        }
        psk_reporter_stop(&psk_reporter);
    } else {
        pending = wspr_reporter_pending(&reporter);
        if (warn_pending && pending != 0U) {
            fprintf(stderr, "Stopping with %zu unsent WSPRnet spots in RAM\n",
                    pending);
        }
        wspr_reporter_stop(&reporter);
    }
    reporting_started = false;
}


/* Thread stuff for separate decoding */
struct decoder_state {
    pthread_t        thread;
    pthread_t        dsp_thread;

    pthread_rwlock_t rw;
    pthread_cond_t   ready_cond;
    pthread_mutex_t  ready_mutex;
    pthread_mutex_t  dsp_mutex;
    struct sample_queue input_queue;
    bool             busy;
    struct timespec  started_at;
};
struct decoder_state dec;


/* Keep libairspyhf's consumer thread short and never wait for DSP work. */
int rx_callback(airspyhf_transfer_t* transfer) {
    if (transfer == NULL || transfer->samples == NULL || transfer->sample_count <= 0) {
        return 0;
    }
    sample_queue_push(&dec.input_queue, transfer->samples,
                      (uint32_t)transfer->sample_count, transfer->dropped_samples);
    return 0;
}

static void process_sample_block(const struct sample_queue_slot *block)
{
    bool frame_complete = false;
    uint64_t input_index = 0;
    uint64_t input_count;
    uint64_t missing_samples;
    const airspyhf_complex_float_t *samples = block->samples;

    if (!rx_state.capture_flag) {
        return;
    }

    pthread_rwlock_wrlock(&dec.rw);
    missing_samples = block->device_dropped_samples + block->queue_dropped_samples;
    input_count = missing_samples + block->sample_count;
    rx_state.device_dropped_samples += block->device_dropped_samples;
    rx_state.queue_dropped_samples += block->queue_dropped_samples;

    for (input_index = 0; input_index < input_count; input_index++) {
        float input_i = 0.0f;
        float input_q = 0.0f;
        float output_i;
        float output_q = 0.0f;
        bool output_ready;

        /* Keep sample time continuous after device or application queue overruns. */
        if (input_index >= missing_samples) {
            uint64_t sample_index = input_index - missing_samples;
            input_i = samples[sample_index].re;
            input_q = samples[sample_index].im;
        }

        if (receiver_mode == RECEIVER_MODE_FT8) {
            output_ready = ft8_resampler_process(&ft8_resampler, input_i, input_q,
                                                 &output_i);
        } else {
            output_ready = wspr_decimator_process(&decimator, input_i, input_q,
                                                  &output_i, &output_q);
        }
        if (output_ready) {
            if (rx_state.iqIndex < capture_sample_target()) {
                rx_state.iSamples[rx_state.iqIndex] = output_i;
                rx_state.qSamples[rx_state.iqIndex] = output_q;
                rx_state.iqIndex++;
            }

            if (rx_state.iqIndex >= capture_sample_target()) {
                rx_state.capture_flag = false;
                frame_complete = true;
                break;
            }
        }
    }

    pthread_rwlock_unlock(&dec.rw);

    if (frame_complete) {
        pthread_mutex_lock(&dec.ready_mutex);
        rx_state.decode_flag = true;
        pthread_cond_signal(&dec.ready_cond);
        pthread_mutex_unlock(&dec.ready_mutex);
    }
}

static void *wsprDsp(void *arg)
{
    struct sample_queue_slot block;

    (void)arg;
    while (sample_queue_wait(&dec.input_queue)) {
        pthread_mutex_lock(&dec.dsp_mutex);
        if (sample_queue_try_peek(&dec.input_queue, &block)) {
            process_sample_block(&block);
            sample_queue_release(&dec.input_queue);
        }
        pthread_mutex_unlock(&dec.dsp_mutex);
    }
    return NULL;
}


void postSpots(uint32_t n_results, const struct decoder_options *frame_options) {
    for (uint32_t i=0; i<n_results; i++) {
        struct wspr_report report = {0};

        printf("Spot : %3.2f %4.2f %10.6f %2d  %-s\n",
               dec_results[i].snr, dec_results[i].dt, dec_results[i].freq,
               (int)dec_results[i].drift, dec_results[i].message);

        if (rx_options.noreport) {
            continue;
        }
        if (!wspr_clock_is_synchronized()) {
            fprintf(stderr, "Clock is unsynchronized; decoded spot will not be reported\n");
            continue;
        }
        snprintf(report.receiver_call, sizeof(report.receiver_call), "%s",
                 frame_options->rcall);
        snprintf(report.receiver_grid, sizeof(report.receiver_grid), "%s",
                 frame_options->rloc);
        snprintf(report.date, sizeof(report.date), "%s", frame_options->date);
        snprintf(report.time, sizeof(report.time), "%s", frame_options->uttime);
        snprintf(report.transmitter_call, sizeof(report.transmitter_call), "%s",
                 dec_results[i].call);
        snprintf(report.transmitter_grid, sizeof(report.transmitter_grid), "%s",
                 dec_results[i].loc);
        snprintf(report.power, sizeof(report.power), "%s", dec_results[i].pwr);
        report.receiver_frequency_mhz = frame_options->freq / 1e6;
        report.transmitter_frequency_mhz = dec_results[i].freq;
        report.snr = dec_results[i].snr;
        report.dt = dec_results[i].dt;
        report.drift = dec_results[i].drift;
        if (!wspr_reporter_enqueue(&reporter, &report)) {
            fprintf(stderr, "Unable to retain WSPRnet spot in RAM queue\n");
        }
    }
    if (n_results == 0) {
        printf("No spot [20%.2s-%.2s-%.2s %.2s:%.2s UTC]\n",
               frame_options->date, frame_options->date + 2, frame_options->date + 4,
               frame_options->uttime, frame_options->uttime + 2);
    }
}


static void *wsprDecoder(void *arg) {
    static float iSamples[MAX_CAPTURE_SAMPLES] = {0};
    static float qSamples[MAX_CAPTURE_SAMPLES] = {0};
    static struct ft8_decode_result ft8_results[FT8_MAX_RESULTS];
    static struct ft8_decode_result jt9_results[FT8_MAX_RESULTS];
    static uint32_t samples_len;
    struct decoder_options frame_options;
    int32_t n_results=0;

    (void)arg;

    while (!rx_state.exit_flag) {
        pthread_mutex_lock(&dec.ready_mutex);
        while (!rx_state.decode_flag && !rx_state.exit_flag) {
            pthread_cond_wait(&dec.ready_cond, &dec.ready_mutex);
        }
        rx_state.decode_flag = false;
        if (!rx_state.exit_flag) {
            dec.busy = true;
            clock_gettime(CLOCK_MONOTONIC, &dec.started_at);
            wspr_health_set_decoder_busy(&health_server, true);
        }
        pthread_mutex_unlock(&dec.ready_mutex);

        if(rx_state.exit_flag)  // Abord case, final sig
            break;

        /* Lock the buffer access and make a local copy */
        pthread_rwlock_wrlock(&dec.rw);
        memcpy(iSamples, rx_state.iSamples, rx_state.iqIndex * sizeof(float));
        memcpy(qSamples, rx_state.qSamples, rx_state.iqIndex * sizeof(float));
        samples_len = rx_state.iqIndex;  // Overkill ?
        uint64_t device_dropped_samples = rx_state.device_dropped_samples;
        uint64_t queue_dropped_samples = rx_state.queue_dropped_samples;
        time_t frame_start_unix = rx_state.frame_start_unix;
        frame_options = dec_options;
        frame_options.freq = rx_state.frame_dialfreq;
        memcpy(frame_options.date, rx_state.frame_date, sizeof(frame_options.date));
        memcpy(frame_options.uttime, rx_state.frame_uttime, sizeof(frame_options.uttime));
        pthread_rwlock_unlock(&dec.rw);

        if (device_dropped_samples != 0) {
            fprintf(stderr, "Warning: inserted %llu zero samples after Airspy overruns\n",
                    (unsigned long long)device_dropped_samples);
        }
        if (queue_dropped_samples != 0) {
            fprintf(stderr, "Warning: inserted %llu zero samples after DSP queue overruns\n",
                    (unsigned long long)queue_dropped_samples);
        }

        bool decode_success;
        uint32_t decoded_count;
        if (receiver_mode == RECEIVER_MODE_FT8) {
            size_t ft8_result_count = 0U;
            size_t jt9_result_count = 0U;
            struct jt9_decode_job jt9_job;
            bool jt9_started = jt9_enabled && jt9_decoder_start(
                &jt9_decoder, iSamples, samples_len, frame_start_unix,
                &jt9_job) == 0;
            bool ft8_lib_success = ft8_decode_frame(
                &ft8_decoder, iSamples, samples_len, ft8_results,
                FT8_MAX_RESULTS, &ft8_result_count) == 0;
            bool jt9_success = jt9_started && jt9_decoder_finish(
                &jt9_decoder, &jt9_job, jt9_results, FT8_MAX_RESULTS,
                &jt9_result_count) == 0;

            if (!ft8_lib_success) {
                fprintf(stderr, "ft8_lib decoder failed for %.6s %.4s\n",
                        frame_options.date, frame_options.uttime);
                ft8_result_count = 0U;
            }
            if (jt9_enabled && !jt9_started) {
                fprintf(stderr,
                        "Unable to start jt9 for %.6s %.2s:%.2s:%02d UTC: %s\n",
                        frame_options.date, frame_options.uttime,
                        frame_options.uttime + 2,
                        (int)(frame_start_unix % 60),
                        jt9_decoder.last_error[0] == '\0' ? "unknown error" :
                                                            jt9_decoder.last_error);
            } else if (jt9_started && !jt9_success) {
                fprintf(stderr,
                        "jt9 failed for %.6s %.2s:%.2s:%02d UTC: %s\n",
                        frame_options.date, frame_options.uttime,
                        frame_options.uttime + 2,
                        (int)(frame_start_unix % 60),
                        jt9_decoder.last_error[0] == '\0' ? "unknown error" :
                                                            jt9_decoder.last_error);
            }
            wspr_health_record_ft8_decoders(
                &health_server, ft8_lib_success, (uint32_t)ft8_result_count,
                jt9_success, (uint32_t)jt9_result_count);
            if (jt9_success) {
                jt9_consecutive_failures = 0U;
                size_t ft8_lib_result_count = ft8_result_count;
                ft8_result_count = jt9_merge_results(
                    ft8_results, ft8_result_count, FT8_MAX_RESULTS,
                    jt9_results, jt9_result_count);
                printf("FT8 decoders: ft8_lib=%zu jt9=%zu unique=%zu\n",
                       ft8_lib_result_count,
                       jt9_result_count, ft8_result_count);
            } else if (jt9_enabled && ++jt9_consecutive_failures >= 3U) {
                fprintf(stderr,
                        "Disabling jt9 after 3 consecutive failures; ft8_lib remains active\n");
                jt9_enabled = false;
                jt9_decoder_free(&jt9_decoder);
            }
            decode_success = ft8_lib_success || jt9_success;
            decoded_count = (uint32_t)ft8_result_count;
            if (decode_success && !rx_state.exit_flag) {
                for (size_t index = 0U; index < ft8_result_count; index++) {
                    printf("FT8  : %+5.1f %+.2f %10.6f  %s\n",
                           ft8_results[index].snr, ft8_results[index].dt,
                           (frame_options.freq +
                            ft8_results[index].audio_frequency_hz) / 1e6,
                           ft8_results[index].message);
                    if (!rx_options.noreport &&
                        ft8_results[index].transmitter_call[0] != '\0') {
                        if (!wspr_clock_is_synchronized()) {
                            fprintf(stderr,
                                    "Clock is unsynchronized; FT8 spot will not be reported\n");
                            continue;
                        }
                        struct psk_report report = {0};
                        long snr = lroundf(ft8_results[index].snr);

                        (void)snprintf(report.sender_call,
                                       sizeof(report.sender_call), "%s",
                                       ft8_results[index].transmitter_call);
                        report.frequency_hz = (uint32_t)lroundf(
                            (float)frame_options.freq +
                            ft8_results[index].audio_frequency_hz);
                        report.snr = (int8_t)(snr < -128L ? -128L :
                                              (snr > 127L ? 127L : snr));
                        report.flow_start = frame_start_unix;
                        if (!psk_reporter_enqueue(&psk_reporter, &report)) {
                            fprintf(stderr,
                                    "Unable to retain FT8 spot in PSK Reporter RAM queue\n");
                        }
                    }
                }
                if (ft8_result_count == 0U) {
                    printf("No FT8 decode [20%.2s-%.2s-%.2s %.2s:%.2s UTC]\n",
                           frame_options.date, frame_options.date + 2,
                           frame_options.date + 4, frame_options.uttime,
                           frame_options.uttime + 2);
                }
            }
        } else {
            /* Keep WSPR arithmetic stable for every HF+ gain setting. */
            float peak = 1e-24f;
            for (uint32_t index = 0U; index < samples_len; index++) {
                peak = fmaxf(peak, fabsf(iSamples[index]));
                peak = fmaxf(peak, fabsf(qSamples[index]));
            }
            float scale = 0.70710678f / peak;
            for (uint32_t index = 0U; index < samples_len; index++) {
                iSamples[index] *= scale;
                qSamples[index] *= scale;
            }
            decode_success = decode_wspr_frame(
                iSamples, qSamples, samples_len, &frame_options,
                dec_results, 50U, &n_results) == 0;
            decoded_count = decode_success ? (uint32_t)n_results : 0U;
            if (!decode_success) {
                fprintf(stderr, "WSPR decoder failed for %.6s %.4s\n",
                        frame_options.date, frame_options.uttime);
            } else if (!rx_state.exit_flag) {
                postSpots(decoded_count, &frame_options);
            }
        }
        wspr_health_record_decode(&health_server, decode_success, decoded_count,
                                  device_dropped_samples,
                                  queue_dropped_samples);
        pthread_mutex_lock(&dec.ready_mutex);
        dec.busy = false;
        pthread_mutex_unlock(&dec.ready_mutex);
        wspr_health_set_decoder_busy(&health_server, false);

    }
    wspr_health_set_decoder_busy(&health_server, false);
    pthread_exit(NULL);
}


double atofs(const char *s) {
    char *end;
    double value;
    double multiplier = 1.0;

    if (s == NULL || *s == '\0') {
        return 0.0;
    }

    value = strtod(s, &end);
    if (end == s) {
        return 0.0;
    }
    if (*end == '\0') {
        return value;
    }
    if (end[1] != '\0') {
        return 0.0;
    }

    switch (*end) {
    case 'g':
    case 'G':
        multiplier = 1e9;
        break;
    case 'm':
    case 'M':
        multiplier = 1e6;
        break;
    case 'k':
    case 'K':
        multiplier = 1e3;
        break;
    default:
        return 0.0;
    }
    return value * multiplier;
}


int32_t parse_u64(char* s, uint64_t* const value) {
    uint_fast8_t base = 10;
    char* s_end;
    uint64_t u64_value;

    if( strlen(s) > 2 ) {
        if( s[0] == '0' ) {
            if( (s[1] == 'x') || (s[1] == 'X') ) {
                base = 16;
                s += 2;
            } else if( (s[1] == 'b') || (s[1] == 'B') ) {
                base = 2;
                s += 2;
            }
        }
    }

    s_end = s;
    u64_value = strtoull(s, &s_end, base);
    if( (s != s_end) && (*s_end == 0) ) {
        *value = u64_value;
        return AIRSPYHF_SUCCESS;
    } else {
        return AIRSPYHF_ERROR;
    }
}


/* Reset flow control variable & decimation variables */
void initSampleStorage(uint32_t frame_dialfreq,
                       time_t frame_start_unix,
                       const char *frame_date,
                       const char *frame_uttime) {
    pthread_mutex_lock(&dec.dsp_mutex);
    pthread_rwlock_wrlock(&dec.rw);
    rx_state.capture_flag = false;
    rx_state.iqIndex = 0;
    rx_state.device_dropped_samples = 0;
    rx_state.queue_dropped_samples = 0;
    rx_state.frame_dialfreq = frame_dialfreq;
    rx_state.frame_start_unix = frame_start_unix;
    snprintf(rx_state.frame_date, sizeof(rx_state.frame_date), "%s", frame_date);
    snprintf(rx_state.frame_uttime, sizeof(rx_state.frame_uttime), "%s", frame_uttime);
    if (receiver_mode == RECEIVER_MODE_FT8) {
        ft8_resampler_reset(&ft8_resampler);
    } else {
        wspr_decimator_reset(&decimator);
    }
    sample_queue_reset(&dec.input_queue);
    rx_state.capture_flag = true;
    pthread_rwlock_unlock(&dec.rw);
    pthread_mutex_unlock(&dec.dsp_mutex);
}

static bool is_capture_active(void) {
    bool active;

    pthread_mutex_lock(&dec.dsp_mutex);
    active = rx_state.capture_flag;
    pthread_mutex_unlock(&dec.dsp_mutex);
    return active;
}

static void health_watchdog_ping(void)
{
    struct timespec now;
    bool decoder_timed_out = false;
    bool clock_synchronized = wspr_clock_is_synchronized();

    wspr_health_set_clock(&health_server, clock_synchronized);

    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&dec.ready_mutex);
    if (dec.busy && now.tv_sec - dec.started_at.tv_sec > 90) {
        decoder_timed_out = true;
    }
    pthread_mutex_unlock(&dec.ready_mutex);
    if (decoder_timed_out) {
        wspr_health_set_receiver_state(&health_server, "decoder_timeout");
        service_notify_status("Decoder deadline exceeded; awaiting watchdog restart");
        return;
    }
    service_watchdog_ping();
}

static void wait_for_synchronized_clock(void)
{
    bool announced = false;

    wspr_health_set_receiver_state(&health_server, "clock_wait");
    while (!rx_state.exit_flag && !wspr_clock_is_synchronized()) {
        wspr_health_set_clock(&health_server, false);
        if (!announced) {
            fprintf(stderr, "Clock is not synchronized; WSPR capture is paused\n");
            service_notify_status("Waiting for synchronized system clock");
            announced = true;
        }
        for (unsigned int step = 0; step < 20 && !rx_state.exit_flag; step++) {
            usleep(250000);
            health_watchdog_ping();
        }
    }
    if (announced && !rx_state.exit_flag) {
        fprintf(stderr, "Clock synchronized; WSPR capture resumed\n");
    }
    if (!rx_state.exit_flag) {
        wspr_health_set_clock(&health_server, true);
        wspr_health_set_receiver_state(&health_server, "waiting");
    }
}


/* Default options for the decoder */
void initDecoder_options() {
    dec_options.usehashtable = 1;
    dec_options.npasses = 2;
    dec_options.subtraction = 1;
    dec_options.quickmode = 0;
}


/* Default options for the receiver */
void initrx_options() {
    rx_options.attenuation = 0;
    rx_options.lna = true;
    rx_options.agc = true;
    rx_options.agc_threshold = true;
    rx_options.noreport = false;
    rx_options.shift = 0;
    rx_options.upconverter = 0;
    rx_options.rate = 192000;
    rx_options.serialnumber = 0;
    rx_options.health_port = 8080;
    snprintf(rx_options.health_bind, sizeof(rx_options.health_bind), "0.0.0.0");
}


void sigint_callback_handler(int signum) {
    (void)signum;
    rx_state.exit_flag = true;
}


void usage(void) {
    fprintf(stderr,
            "airspyhf-wsprd, a WSPR and FT8 decoder for Airspy HF+ receivers\n\n"
            "Use:\tairspyhf-wsprd (-b band | -B bands | -f frequency) -c callsign -g locator [options]\n"
            "\t--mode WSPR|FT8 (default: WSPR)\n"
            "\t-b mode-specific band preset (for example: 20m)\n"
            "\t   WSPR bands: %s\n"
            "\t   FT8 bands : %s\n"
            "\t-B comma-separated bands (FT8: 2 minutes; WSPR: 10 minutes)\n"
            "\t   'auto' uses wspr.live for WSPR or PSK Reporter for FT8\n"
            "\t-f dial frequency [(,k,M) Hz], manual alternative to -b\n"
            "\t-c your callsign (12 chars max)\n"
            "\t-g your locator grid (6 chars max)\n"
            "Receiver extra options:\n"
            "\t-r sampling rate (default: 192k; must divide evenly to the decoder rate)\n"
            "\t-A HF AGC [0-1] (default: 1 enabled)\n"
            "\t-t HF AGC threshold [0=low, 1=high] (default: high/sensitive)\n"
            "\t-a attenuation in dB [0,6,...,48] when AGC is off (default: 0)\n"
            "\t-l HF preamp/postamp [0-1] (default: 1 for maximum sensitivity)\n"
            "\t-p frequency correction in Hz (default: 0)\n"
            "\t-u upconverter (default: 0, example: 125M)\n"
            "\t-s S/N: Open device with specified 64bits serial number\n"
            "\t-e health HTTP bind address (default: 0.0.0.0)\n"
            "\t-P health HTTP port (default: 8080; 0 disables)\n"
            "\t-n decode and print spots without uploading to WSPRnet or PSK Reporter\n"
            "Decoder extra options:\n"
            "\t-H do not use (or update) the hash table\n"
            "\t-Q quick mode, doesn't dig deep for weak signals\n"
            "\t-S single pass mode, no subtraction (same as original wsprd)\n"
            "Example:\n"
            "\tairspyhf-wsprd -b 20m -c VU3CER -g MK68xm\n"
            "\tairspyhf-wsprd --mode FT8 -b 20m -c VU3CER -g MK68xm\n",
            wspr_supported_bands(), ft8_supported_bands());
    exit(1);
}

static bool validate_sample_rate(airspyhf_device_t *receiver, uint32_t requested_rate) {
    uint32_t count = 0;
    uint32_t *rates;
    bool found = false;

    if (airspyhf_get_samplerates(receiver, &count, 0) != AIRSPYHF_SUCCESS || count == 0) {
        fprintf(stderr, "airspyhf_get_samplerates() failed\n");
        return false;
    }

    rates = calloc(count, sizeof(*rates));
    if (rates == NULL) {
        fprintf(stderr, "Unable to allocate sample-rate list\n");
        return false;
    }
    if (airspyhf_get_samplerates(receiver, rates, count) != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "airspyhf_get_samplerates() failed\n");
        free(rates);
        return false;
    }

    fprintf(stderr, "HF+ sample rates:");
    for (uint32_t i = 0; i < count; i++) {
        fprintf(stderr, " %u", rates[i]);
        if (rates[i] == requested_rate) {
            found = true;
        }
    }
    fprintf(stderr, " Hz\n");
    free(rates);

    if (!found) {
        fprintf(stderr, "Requested sample rate %u Hz is not supported by this receiver.\n",
                requested_rate);
    }
    return found;
}

static bool calculate_receiver_frequency(uint32_t dial_frequency,
                                         uint32_t *receiver_frequency)
{
    int64_t tuned_frequency = (int64_t)dial_frequency + rx_options.shift +
                              rx_options.upconverter;
    int64_t center_frequency = tuned_frequency + 1500;

    if (receiver_frequency == NULL || tuned_frequency <= 0 ||
        center_frequency > UINT32_MAX ||
        !((center_frequency >= 9000 && center_frequency <= 31000000) ||
          (center_frequency >= 60000000 && center_frequency <= 260000000))) {
        return false;
    }
    *receiver_frequency = (uint32_t)tuned_frequency;
    return true;
}

static bool tune_hop_band(const struct wspr_hop_band *band)
{
    uint32_t receiver_frequency;
    int result;

    if (band == NULL ||
        !calculate_receiver_frequency(band->frequency_hz, &receiver_frequency)) {
        return false;
    }
    result = airspyhf_set_freq(device, receiver_frequency + 1500U);
    if (result != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "airspyhf_set_freq(%u) failed (%d)\n",
                receiver_frequency + 1500U, result);
        return false;
    }
    rx_options.dialfreq = band->frequency_hz;
    rx_options.realfreq = receiver_frequency;
    snprintf(rx_options.band, sizeof(rx_options.band), "%s", band->name);
    printf("Band hop: %s, dial %u Hz, IQ center %u Hz\n",
           band->name, band->frequency_hz, receiver_frequency + 1500U);
    return true;
}

static void print_hop_plan(const char *label, const struct wspr_hop_plan *plan)
{
    printf("  %-13s:", label);
    for (size_t index = 0; index < plan->count; index++) {
        printf("%s%s", index == 0U ? " " : ",", plan->bands[index].name);
    }
    printf("\n");
}


int main(int argc, char** argv) {
    enum { OPTION_MODE = 1000 };
    static const struct option long_options[] = {
        {"mode", required_argument, NULL, OPTION_MODE},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    int result;
    int output_size;
    uint32_t exit_code = EXIT_SUCCESS;
    bool band_selected = false;
    bool frequency_selected = false;
    bool hopping_selected = false;
    bool adaptive_hopping = false;
    bool advice_available = false;
    uint32_t hop_dwell_seconds = WSPR_HOP_DWELL_SECONDS;
    char hopping_spec[256] = "";

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    initrx_options();
    initDecoder_options();

    /* RX buffer allocation */
    rx_state.iSamples = malloc(sizeof(float) * MAX_CAPTURE_SAMPLES);
    rx_state.qSamples = malloc(sizeof(float) * MAX_CAPTURE_SAMPLES);
    if (rx_state.iSamples == NULL || rx_state.qSamples == NULL) {
        fprintf(stderr, "Unable to allocate receiver buffers\n");
        free(rx_state.iSamples);
        free(rx_state.qSamples);
        return EXIT_FAILURE;
    }

    /* Stop condition setup */
    rx_state.exit_flag   = false;
    rx_state.decode_flag = false;

    if (argc <= 1)
        usage();

    while ((opt = getopt_long(argc, argv,
                              "b:B:f:c:g:r:a:l:A:t:s:p:u:e:P:nHQSh",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'b': // Mode-specific band preset
            if (snprintf(rx_options.band, sizeof(rx_options.band), "%s", optarg) >=
                (int)sizeof(rx_options.band)) {
                fprintf(stderr, "Band name is too long.\n");
                return EXIT_FAILURE;
            }
            band_selected = true;
            break;
        case 'B': // Mode-specific band hopping
            adaptive_hopping = strcasecmp(optarg, "auto") == 0;
            if (snprintf(hopping_spec, sizeof(hopping_spec), "%s", optarg) >=
                (int)sizeof(hopping_spec)) {
                fprintf(stderr, "Band hopping list is too long.\n");
                return EXIT_FAILURE;
            }
            hopping_selected = true;
            break;
        case 'f': // Frequency
            rx_options.dialfreq = (uint32_t)atofs(optarg);
            frequency_selected = true;
            break;
        case 'c': // Callsign
            if (!station_normalize_callsign(dec_options.rcall,
                                            sizeof(dec_options.rcall), optarg)) {
                fprintf(stderr, "Invalid callsign '%s'. Use 3-12 letters, digits, or '/'.\n",
                        optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'g': // Locator / Grid
            if (!station_normalize_grid(dec_options.rloc,
                                        sizeof(dec_options.rloc), optarg)) {
                fprintf(stderr, "Invalid Maidenhead grid '%s'. Use 4 or 6 characters.\n",
                        optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'r': // sampling rate
            rx_options.rate = (uint32_t)atofs(optarg);
            break;
        case 'a': // HF attenuation
            rx_options.attenuation = (uint32_t)atoi(optarg);
            if (rx_options.attenuation > 48 || rx_options.attenuation % 6 != 0) {
                fprintf(stderr, "Attenuation must be one of 0, 6, ..., 48 dB.\n");
                return EXIT_FAILURE;
            }
            break;
        case 'l': // HF LNA
            rx_options.lna = atoi(optarg) != 0;
            break;
        case 'A': // HF AGC
            rx_options.agc = atoi(optarg) != 0;
            break;
        case 't': // HF AGC threshold
            rx_options.agc_threshold = atoi(optarg) != 0;
            break;
        case 's': // Serial number
            if (parse_u64(optarg, &rx_options.serialnumber) != AIRSPYHF_SUCCESS) {
                fprintf(stderr, "Invalid serial number: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'p': // Fine frequency correction
            rx_options.shift = (int32_t)atoi(optarg);
            break;
        case 'u': // Upconverter frequency
            rx_options.upconverter = (uint32_t)atofs(optarg);
            break;
        case 'e': { // Health server IPv4 bind address
            struct in_addr address;
            if (inet_pton(AF_INET, optarg, &address) != 1 ||
                snprintf(rx_options.health_bind, sizeof(rx_options.health_bind),
                         "%s", optarg) >= (int)sizeof(rx_options.health_bind)) {
                fprintf(stderr, "Health bind address must be a numeric IPv4 address.\n");
                return EXIT_FAILURE;
            }
            break;
        }
        case 'P': { // Health server TCP port
            char *end;
            long port = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || port < 0 || port > UINT16_MAX) {
                fprintf(stderr, "Health port must be between 0 and 65535.\n");
                return EXIT_FAILURE;
            }
            rx_options.health_port = (uint16_t)port;
            break;
        }
        case OPTION_MODE:
            if (strcasecmp(optarg, "WSPR") == 0) {
                receiver_mode = RECEIVER_MODE_WSPR;
            } else if (strcasecmp(optarg, "FT8") == 0) {
                receiver_mode = RECEIVER_MODE_FT8;
            } else {
                fprintf(stderr, "Mode must be WSPR or FT8.\n");
                return EXIT_FAILURE;
            }
            break;
        case 'n': // Do not upload spots
            rx_options.noreport = true;
            break;
        case 'H': // Decoder option, use a hastable
            dec_options.usehashtable = 0;
            break;
        case 'Q': // Decoder option, faster
            dec_options.quickmode = 1;
            break;
        case 'S': // Decoder option, single pass mode (same as original wsprd)
            dec_options.subtraction = 0;
            dec_options.npasses = 1;
            break;
        case 'h':
            usage();
            break;
        default:
            usage();
            break;
        }
    }

    hop_dwell_seconds = receiver_mode == RECEIVER_MODE_FT8 ?
                        FT8_HOP_DWELL_SECONDS : WSPR_HOP_DWELL_SECONDS;

    if (band_selected && frequency_selected) {
        fprintf(stderr, "Use either -b for a band preset or -f for a dial frequency, not both.\n");
        return EXIT_FAILURE;
    }
    if (hopping_selected && frequency_selected) {
        fprintf(stderr, "Use either -B for band hopping or -f for a dial frequency, not both.\n");
        return EXIT_FAILURE;
    }
    if (hopping_selected) {
        const char *bands = adaptive_hopping ? automatic_hop_bands : hopping_spec;
        bool valid_plan = receiver_mode == RECEIVER_MODE_FT8 ?
                          ft8_hop_plan_parse(&hop_plan, bands) :
                          wspr_hop_plan_parse(&hop_plan, bands);
        if (!valid_plan) {
            fprintf(stderr,
                    "Invalid %s hopping list '%s'. Use at least two unique bands, or 'auto'.\n"
                    "Supported bands: %s\n",
                    receiver_mode == RECEIVER_MODE_FT8 ? "FT8" : "WSPR",
                    hopping_spec,
                    receiver_mode == RECEIVER_MODE_FT8 ? ft8_supported_bands() :
                                                        wspr_supported_bands());
            return EXIT_FAILURE;
        }
    }

    if (band_selected) {
        bool valid_band = receiver_mode == RECEIVER_MODE_FT8 ?
                          ft8_band_frequency(rx_options.band,
                                             &rx_options.dialfreq) :
                          wspr_band_frequency(rx_options.band,
                                              &rx_options.dialfreq);
        if (!valid_band) {
            fprintf(stderr, "Unknown or untunable %s band '%s'.\nSupported bands: %s\n",
                    receiver_mode == RECEIVER_MODE_FT8 ? "FT8" : "WSPR",
                    rx_options.band,
                    receiver_mode == RECEIVER_MODE_FT8 ? ft8_supported_bands() :
                                                        wspr_supported_bands());
            return EXIT_FAILURE;
        }
    }

    if (hopping_selected) {
        rx_options.dialfreq = hop_plan.bands[0].frequency_hz;
        snprintf(rx_options.band, sizeof(rx_options.band), "%s", hop_plan.bands[0].name);
    }

    if (rx_options.dialfreq == 0) {
        fprintf(stderr, "Please specify a band with -b or a dial frequency with -f.\n");
        fprintf(stderr, " --help for usage...\n");
        exit(1);
    }

    if (dec_options.rcall[0] == 0) {
        fprintf(stderr, "Please specify your callsign.\n");
        fprintf(stderr, " --help for usage...\n");
        exit(1);
    }

    if (dec_options.rloc[0] == 0) {
        fprintf(stderr, "Please specify your locator.\n");
        fprintf(stderr, " --help for usage...\n");
        exit(1);
    }

    uint32_t decoder_rate;
    if (receiver_mode == RECEIVER_MODE_FT8) {
        decoder_rate = FT8_AUDIO_RATE;
        if (!ft8_resampler_init(&ft8_resampler, rx_options.rate)) {
            fprintf(stderr, "FT8 sample rate %u must be exactly divisible by %u Hz.\n",
                    rx_options.rate, FT8_AUDIO_RATE);
            return EXIT_FAILURE;
        }
    } else {
        decoder_rate = WSPR_OUTPUT_RATE;
        if (!wspr_decimator_init(&decimator, rx_options.rate)) {
            fprintf(stderr, "WSPR sample rate %u must be exactly divisible by %u Hz.\n",
                    rx_options.rate, WSPR_OUTPUT_RATE);
            return EXIT_FAILURE;
        }
    }
    rx_options.downsampling = rx_options.rate / decoder_rate;
    uint32_t receiver_frequency;
    if (!calculate_receiver_frequency(rx_options.dialfreq, &receiver_frequency)) {
        fprintf(stderr, "HF+ center frequency for %u Hz is outside its supported ranges.\n",
                rx_options.dialfreq);
        return EXIT_FAILURE;
    }
    rx_options.realfreq = receiver_frequency;

    for (size_t index = 0; index < hop_plan.count; index++) {
        uint32_t unused_frequency;
        if (!calculate_receiver_frequency(hop_plan.bands[index].frequency_hz,
                                          &unused_frequency)) {
            fprintf(stderr, "Hopping band %s is outside the HF+ tuning ranges.\n",
                    hop_plan.bands[index].name);
            return EXIT_FAILURE;
        }
    }

    /* Store the frequency used for the decoder */
    dec_options.freq = rx_options.dialfreq;

    /* If something goes wrong... */
    signal(SIGINT, &sigint_callback_handler);
    signal(SIGTERM, &sigint_callback_handler);

    if( rx_options.serialnumber ) {
        result = airspyhf_open_sn(&device, rx_options.serialnumber);
        if( result != AIRSPYHF_SUCCESS ) {
            fprintf(stderr, "airspyhf_open_sn() failed (%d)\n", result);
            return EXIT_FAILURE;
        }
    } else {
        result = airspyhf_open(&device);
        if( result != AIRSPYHF_SUCCESS ) {
            fprintf(stderr, "airspyhf_open() failed (%d)\n", result);
            return EXIT_FAILURE;
        }
    }

    if (!validate_sample_rate(device, rx_options.rate)) {
        airspyhf_close(device);
        return EXIT_FAILURE;
    }

    result = airspyhf_set_samplerate(device, rx_options.rate);
    if (result != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "airspyhf_set_samplerate(%u) failed (%d)\n", rx_options.rate, result);
        airspyhf_close(device);
        return EXIT_FAILURE;
    }

    result = airspyhf_set_hf_agc(device, rx_options.agc ? 1 : 0);
    if (result != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "airspyhf_set_hf_agc() failed (%d)\n", result);
        airspyhf_close(device);
        return EXIT_FAILURE;
    }

    if (rx_options.agc) {
        result = airspyhf_set_hf_agc_threshold(device, rx_options.agc_threshold ? 1 : 0);
    } else {
        result = airspyhf_set_hf_att(device, (uint8_t)(rx_options.attenuation / 6));
    }
    if (result != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "Unable to configure HF+ AGC threshold/attenuator (%d)\n", result);
        airspyhf_close(device);
        return EXIT_FAILURE;
    }

    result = airspyhf_set_hf_lna(device, rx_options.lna ? 1 : 0);
    if (result != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "airspyhf_set_hf_lna() failed (%d)\n", result);
        airspyhf_close(device);
        return EXIT_FAILURE;
    }

    /* libairspyhf returns complex IQ centred on this frequency. */
    result = airspyhf_set_freq(device, rx_options.realfreq + 1500);
    if (result != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "airspyhf_set_freq() failed (%d)\n", result);
        airspyhf_close(device);
        return EXIT_FAILURE;
    }

    result = airspyhf_board_partid_serialno_read(device, &readSerial);
    if (result != AIRSPYHF_SUCCESS) {
        fprintf(stderr, "airspyhf_board_partid_serialno_read() failed (%d)\n", result);
        airspyhf_close(device);
        return EXIT_FAILURE;
    }

    pthread_rwlock_init(&dec.rw, NULL);
    pthread_cond_init(&dec.ready_cond, NULL);
    pthread_mutex_init(&dec.ready_mutex, NULL);
    pthread_mutex_init(&dec.dsp_mutex, NULL);
    output_size = airspyhf_get_output_size(device);
    if (output_size <= 0 ||
        !sample_queue_init(&dec.input_queue, sizeof(airspyhf_complex_float_t),
                           (uint32_t)output_size, DSP_QUEUE_SLOTS)) {
        fprintf(stderr, "Unable to allocate the RAM-only DSP input queue\n");
        airspyhf_close(device);
        pthread_rwlock_destroy(&dec.rw);
        pthread_cond_destroy(&dec.ready_cond);
        pthread_mutex_destroy(&dec.ready_mutex);
        pthread_mutex_destroy(&dec.dsp_mutex);
        return EXIT_FAILURE;
    }
    if (start_reporting() != 0) {
        fprintf(stderr, "Unable to start the reporting RAM queue\n");
        airspyhf_close(device);
        pthread_rwlock_destroy(&dec.rw);
        pthread_cond_destroy(&dec.ready_cond);
        pthread_mutex_destroy(&dec.ready_mutex);
        pthread_mutex_destroy(&dec.dsp_mutex);
        sample_queue_destroy(&dec.input_queue);
        return EXIT_FAILURE;
    }
    if (receiver_mode == RECEIVER_MODE_FT8 && !ft8_decoder_init(&ft8_decoder)) {
        fprintf(stderr, "Unable to initialize the FT8 decoder\n");
        stop_reporting(false);
        airspyhf_close(device);
        pthread_rwlock_destroy(&dec.rw);
        pthread_cond_destroy(&dec.ready_cond);
        pthread_mutex_destroy(&dec.ready_mutex);
        pthread_mutex_destroy(&dec.dsp_mutex);
        sample_queue_destroy(&dec.input_queue);
        return EXIT_FAILURE;
    }
    if (receiver_mode == RECEIVER_MODE_FT8) {
        const char *jt9_path = getenv("AIRSPYHF_WSPRD_JT9");
        if (jt9_path == NULL) {
            jt9_path = "/usr/local/libexec/airspyhf-wsprd-jt9";
        }
        if (jt9_path[0] != '\0') {
            jt9_enabled = jt9_decoder_init(&jt9_decoder, jt9_path,
                                           dec_options.rcall,
                                           dec_options.rloc);
            if (!jt9_enabled && getenv("AIRSPYHF_WSPRD_JT9") != NULL) {
                fprintf(stderr,
                        "Configured jt9 is unavailable; using ft8_lib only\n");
            }
            if (jt9_enabled) {
                fprintf(stderr,
                        "Warming up jt9 in RAM (deadline %u seconds)\n",
                        JT9_WARMUP_TIMEOUT_SECONDS);
                if (!jt9_decoder_warmup(&jt9_decoder)) {
                    fprintf(stderr, "jt9 warm-up failed: %s; using ft8_lib only\n",
                            jt9_decoder.last_error[0] == '\0' ? "unknown error" :
                                                                jt9_decoder.last_error);
                    jt9_decoder_free(&jt9_decoder);
                    jt9_enabled = false;
                } else {
                    fprintf(stderr, "jt9 warm-up complete\n");
                }
            }
        }
    }
    pthread_attr_t decoder_attributes;
    pthread_attr_init(&decoder_attributes);
    pthread_attr_setstacksize(&decoder_attributes, 4U * 1024U * 1024U);
    result = pthread_create(&dec.thread, &decoder_attributes, wsprDecoder, NULL);
    pthread_attr_destroy(&decoder_attributes);
    if (result != 0) {
        fprintf(stderr, "Unable to create decoder thread (%d)\n", result);
        stop_reporting(false);
        ft8_decoder_free(&ft8_decoder);
        jt9_decoder_free(&jt9_decoder);
        airspyhf_close(device);
        pthread_rwlock_destroy(&dec.rw);
        pthread_cond_destroy(&dec.ready_cond);
        pthread_mutex_destroy(&dec.ready_mutex);
        pthread_mutex_destroy(&dec.dsp_mutex);
        sample_queue_destroy(&dec.input_queue);
        return EXIT_FAILURE;
    }
    result = pthread_create(&dec.dsp_thread, NULL, wsprDsp, NULL);
    if (result != 0) {
        fprintf(stderr, "Unable to create DSP thread (%d)\n", result);
        rx_state.exit_flag = true;
        pthread_mutex_lock(&dec.ready_mutex);
        pthread_cond_signal(&dec.ready_cond);
        pthread_mutex_unlock(&dec.ready_mutex);
        pthread_join(dec.thread, NULL);
        stop_reporting(false);
        ft8_decoder_free(&ft8_decoder);
        jt9_decoder_free(&jt9_decoder);
        airspyhf_close(device);
        sample_queue_destroy(&dec.input_queue);
        pthread_rwlock_destroy(&dec.rw);
        pthread_cond_destroy(&dec.ready_cond);
        pthread_mutex_destroy(&dec.ready_mutex);
        pthread_mutex_destroy(&dec.dsp_mutex);
        return EXIT_FAILURE;
    }

    /* Stream continuously for receiver stability; callbacks store aligned frames only. */
    result = airspyhf_start(device, rx_callback, NULL);
    if( result != AIRSPYHF_SUCCESS ) {
        fprintf(stderr, "airspyhf_start() failed (%d)\n", result);
        rx_state.exit_flag = true;
        pthread_mutex_lock(&dec.ready_mutex);
        pthread_cond_signal(&dec.ready_cond);
        pthread_mutex_unlock(&dec.ready_mutex);
        sample_queue_stop(&dec.input_queue);
        pthread_join(dec.dsp_thread, NULL);
        pthread_join(dec.thread, NULL);
        stop_reporting(false);
        ft8_decoder_free(&ft8_decoder);
        jt9_decoder_free(&jt9_decoder);
        airspyhf_close(device);
        sample_queue_destroy(&dec.input_queue);
        pthread_rwlock_destroy(&dec.rw);
        pthread_cond_destroy(&dec.ready_cond);
        pthread_mutex_destroy(&dec.ready_mutex);
        pthread_mutex_destroy(&dec.dsp_mutex);
        return EXIT_FAILURE;
    }
    if (adaptive_hopping) {
        int advisor_result = receiver_mode == RECEIVER_MODE_FT8 ?
                             ft8_band_advisor_start(&band_advisor,
                                                    dec_options.rloc, &hop_plan) :
                             wspr_band_advisor_start(&band_advisor,
                                                    dec_options.rloc, &hop_plan);
        if (advisor_result != 0) {
            fprintf(stderr, "Unable to start %s band advisor; using local schedule\n",
                    receiver_mode == RECEIVER_MODE_FT8 ? "PSK Reporter" :
                                                        "wspr.live");
            adaptive_hopping = false;
        } else if (receiver_mode == RECEIVER_MODE_FT8) {
            fprintf(stderr,
                    "Waiting up to %u seconds for initial PSK Reporter band advice\n",
                    FT8_BOOT_ADVICE_TIMEOUT_SECONDS);
            if (wspr_band_advisor_wait(&band_advisor, &advised_hop_plan,
                                       FT8_BOOT_ADVICE_TIMEOUT_SECONDS)) {
                advice_available = true;
                fprintf(stderr, "Initial PSK Reporter band advice is active\n");
            } else {
                fprintf(stderr,
                        "Initial PSK Reporter band advice unavailable; starting with local schedule\n");
            }
        }
    }
    if (rx_options.health_port != 0U) {
        if (wspr_health_server_start(&health_server, rx_options.health_bind,
                                     rx_options.health_port,
                                     receiver_mode == RECEIVER_MODE_FT8 ? "FT8" : "WSPR",
                                     dec_options.rcall,
                                     dec_options.rloc, !rx_options.noreport,
                                     hopping_selected, adaptive_hopping,
                                     health_report_queue_depth, NULL) != 0) {
            fprintf(stderr,
                    "Warning: health HTTP server could not bind to %s:%u; reception continues\n",
                    rx_options.health_bind, rx_options.health_port);
        } else {
            wspr_health_set_clock(&health_server, wspr_clock_is_synchronized());
            wspr_health_set_receiver_state(&health_server, "waiting");
            if (hopping_selected) {
                wspr_health_set_selected_bands(
                    &health_server,
                    adaptive_hopping && advice_available ?
                        &advised_hop_plan : &hop_plan);
            }
            wspr_health_set_tuning(&health_server,
                                   rx_options.band[0] == '\0' ? "manual" : rx_options.band,
                                   rx_options.dialfreq, false);
        }
    }
    service_notify_ready();

    /* Print used parameter */
    time_t rawtime;
    time ( &rawtime );
    struct tm *gtm = gmtime(&rawtime);
    printf("\nStarting airspyhf-wsprd (%04d-%02d-%02d, %02d:%02dz) - Version 0.9\n",
           gtm->tm_year + 1900, gtm->tm_mon + 1, gtm->tm_mday, gtm->tm_hour, gtm->tm_min);
    printf("  Mode         : %s\n",
           receiver_mode == RECEIVER_MODE_FT8 ? "FT8" : "WSPR");
    printf("  Callsign     : %s\n", dec_options.rcall);
    printf("  Locator      : %s\n", dec_options.rloc);
    if (rx_options.band[0] != '\0' && !hopping_selected) {
        printf("  Band         : %s\n", rx_options.band);
    }
    if (hopping_selected) {
        print_hop_plan("Hop fallback", &hop_plan);
        if (adaptive_hopping && advice_available) {
            print_hop_plan("Hop active", &advised_hop_plan);
        }
        printf("  Hop interval : %u minutes\n", hop_dwell_seconds / 60U);
        printf("  Band advice  : %s\n",
               adaptive_hopping ?
                   (receiver_mode == RECEIVER_MODE_FT8 ?
                        "PSK Reporter active monitors, RAM-only" :
                        "wspr.live, regional, RAM-only") :
                   "fixed local schedule");
        if (adaptive_hopping) {
            printf("  Exploration  : one rotating band every 6 hops\n");
        }
    }
    printf("  Dial freq.   : %d Hz\n", rx_options.dialfreq);
    printf("  IQ center    : %d Hz\n", rx_options.realfreq + 1500);
    printf("  Rate         : %d Hz\n", rx_options.rate);
    printf("  Decimation   : %d\n", rx_options.downsampling);
    printf("  HF AGC       : %s\n", rx_options.agc ? "yes" : "no");
    printf("  AGC threshold: %s\n", rx_options.agc_threshold ? "high" : "low");
    printf("  Attenuation  : %u dB%s\n", rx_options.attenuation,
           rx_options.agc ? " (ignored while AGC is enabled)" : "");
    printf("  Preamp       : %s\n", rx_options.lna ? "yes" : "no");
    if (receiver_mode == RECEIVER_MODE_FT8) {
        printf("  FT8 decoders : %s\n",
               jt9_enabled ? "ft8_lib + jt9 parallel" : "ft8_lib");
        printf("  PSK Reporter : %s\n",
               rx_options.noreport ? "upload disabled" : "upload enabled");
    } else {
        printf("  WSPRnet      : %s\n",
               rx_options.noreport ? "upload disabled" : "upload enabled");
    }
    if (health_server.started) {
        printf("  Health HTTP  : http://%s:%u/health\n",
               health_server.bind_address, health_server.port);
    } else {
        printf("  Health HTTP  : disabled or unavailable\n");
    }
    printf("  S/N          : 0x%08X%08X\n", readSerial.serial_no[0], readSerial.serial_no[1]);

    /* Time alignment stuff */
    struct timeval lTime;
    uint32_t slot_seconds = receiver_mode == RECEIVER_MODE_FT8 ? 15U : 120U;
    uint32_t slot_microseconds = slot_seconds * 1000000U;
    gettimeofday(&lTime, NULL);
    uint32_t sec   = (uint32_t)(lTime.tv_sec % slot_seconds);
    uint32_t usec  = sec * 1000000 + lTime.tv_usec;
    uint32_t uwait = slot_microseconds - usec;
    printf("Wait for time sync (start in %d sec)\n\n", uwait/1000000);

    /* Main loop : Wait, read, decode */
    while (!rx_state.exit_flag) {
        wait_for_synchronized_clock();
        if (rx_state.exit_flag) {
            break;
        }
        service_notify_status("Clock synchronized; waiting for next frame");
        /* Select the band before waiting for the next decoder frame. */
        gettimeofday(&lTime, NULL);
        time_t next_frame_start =
            (lTime.tv_sec / slot_seconds + 1) * slot_seconds;
        if (adaptive_hopping && next_frame_start % hop_dwell_seconds == 0) {
            struct wspr_hop_plan advised_plan;
            if (wspr_band_advisor_take(&band_advisor, &advised_plan)) {
                advised_hop_plan = advised_plan;
                advice_available = true;
                print_hop_plan("Hop active", &advised_hop_plan);
                wspr_health_set_selected_bands(&health_server,
                                               &advised_hop_plan);
            }
        }
        if (hopping_selected) {
            bool exploration_slot = false;
            const struct wspr_hop_band *next_band;

            if (adaptive_hopping && advice_available) {
                next_band = wspr_adaptive_hop_band_at_dwell(
                    &advised_hop_plan, &hop_plan, next_frame_start,
                    hop_dwell_seconds, &exploration_slot);
            } else {
                next_band = wspr_hop_band_at_dwell(
                    &hop_plan, next_frame_start, hop_dwell_seconds);
            }
            if (next_band == NULL) {
                fprintf(stderr, "Unable to select the next hopping band\n");
                rx_state.exit_flag = true;
                exit_code = EXIT_FAILURE;
                break;
            }
            if (exploration_slot &&
                next_frame_start % hop_dwell_seconds == 0) {
                printf("Band exploration: %s\n", next_band->name);
            }
            if (next_band->frequency_hz != rx_options.dialfreq &&
                !tune_hop_band(next_band)) {
                fprintf(stderr, "Unable to tune the next hopping band\n");
                rx_state.exit_flag = true;
                exit_code = EXIT_FAILURE;
                break;
            }
            wspr_health_set_tuning(&health_server, next_band->name,
                                   next_band->frequency_hz, exploration_slot);
        }
        sec   = (uint32_t)(lTime.tv_sec % slot_seconds);
        usec  = sec * 1000000 + lTime.tv_usec;
        uwait = slot_microseconds - usec + 10000U;
        while (uwait > 0 && !rx_state.exit_flag) {
            uint32_t sleep_time = uwait > 250000 ? 250000 : uwait;
            usleep(sleep_time);
            health_watchdog_ping();
            uwait -= sleep_time;
        }
        if (rx_state.exit_flag) {
            break;
        }
        //printf("SYNC! RX started\n");

        /* Use the Store the date at the begin of the frame */
        time ( &rawtime );
        gtm = gmtime(&rawtime);
        char full_date[9];
        char frame_date[7];
        char frame_uttime[5];
        if (gtm == NULL ||
            strftime(full_date, sizeof(full_date), "%Y%m%d", gtm) != 8U ||
            strftime(frame_uttime, sizeof(frame_uttime), "%H%M", gtm) != 4U) {
            fprintf(stderr, "Unable to format the current UTC time\n");
            rx_state.exit_flag = true;
            exit_code = EXIT_FAILURE;
            break;
        }
        memcpy(frame_date, full_date + 2, 6);
        frame_date[6] = '\0';

        /* Start to store the samples */
        service_notify_status("Capturing receiver frame");
        wspr_health_set_receiver_state(&health_server, "capturing");
        initSampleStorage(rx_options.dialfreq, rawtime, frame_date, frame_uttime);

        while (airspyhf_is_streaming(device) &&
               !rx_state.exit_flag &&
               is_capture_active()) {
            usleep(250000);
            health_watchdog_ping();
        }

        if (!airspyhf_is_streaming(device) && !rx_state.exit_flag) {
            fprintf(stderr, "Airspy HF+ stream stopped unexpectedly\n");
            wspr_health_set_receiver_state(&health_server, "error");
            rx_state.exit_flag = true;
            exit_code = EXIT_FAILURE;
        } else if (!rx_state.exit_flag) {
            wspr_health_set_receiver_state(&health_server, "waiting");
        }
    }

    wspr_health_set_receiver_state(&health_server, "stopping");
    wspr_band_advisor_stop(&band_advisor);
    result = airspyhf_stop(device);
    if( result != AIRSPYHF_SUCCESS ) {
        fprintf(stderr, "airspyhf_stop() failed (%d)\n", result);
        exit_code = EXIT_FAILURE;
    }

    if(device != NULL) {
        result = airspyhf_close(device);
        if( result != AIRSPYHF_SUCCESS ) {
            fprintf(stderr, "airspyhf_close() failed (%d)\n", result);
            exit_code = EXIT_FAILURE;
        }
    }

    printf("Bye!\n");
    service_notify_status("Stopping");

    /* Wait the thread join (send a signal before to terminate the job) */
    sample_queue_stop(&dec.input_queue);
    pthread_join(dec.dsp_thread, NULL);
    pthread_mutex_lock(&dec.ready_mutex);
    pthread_cond_signal(&dec.ready_cond);
    pthread_mutex_unlock(&dec.ready_mutex);
    pthread_join(dec.thread, NULL);
    wspr_health_server_stop(&health_server);
    stop_reporting(true);
    ft8_decoder_free(&ft8_decoder);
    jt9_decoder_free(&jt9_decoder);

    /* Destroy the lock/cond/thread */
    sample_queue_destroy(&dec.input_queue);
    pthread_rwlock_destroy(&dec.rw);
    pthread_cond_destroy(&dec.ready_cond);
    pthread_mutex_destroy(&dec.ready_mutex);
    pthread_mutex_destroy(&dec.dsp_mutex);

    free(rx_state.iSamples);
    free(rx_state.qSamples);

    return exit_code;
}
