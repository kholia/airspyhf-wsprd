/*
 * FreeBSD License
 * Copyright (c) 2016, Guenael
 * All rights reserved.
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


#pragma once

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

struct receiver_state {
    /* Variables used for stop conditions */
    volatile sig_atomic_t exit_flag;
    bool decode_flag;
    bool capture_flag;

    /* Buffer used for sampling */
    float *iSamples;
    float *qSamples;

    /* Simple index */
    uint32_t iqIndex;
    uint64_t device_dropped_samples;
    uint64_t queue_dropped_samples;
    uint32_t frame_dialfreq;
    time_t frame_start_unix;
    char frame_date[7];
    char frame_uttime[5];
};


/* Option & config of the receiver */
struct receiver_options {
    uint32_t dialfreq;
    uint32_t realfreq;
    uint32_t attenuation;
    bool     lna;
    bool     agc;
    bool     agc_threshold;
    bool     noreport;
    int32_t  shift;
    uint32_t upconverter;
    uint32_t rate;
    uint32_t downsampling;
    uint64_t serialnumber;
    uint16_t health_port;
    char     health_bind[16];
    char     band[8];
    char     date[7];
    char     uttime[5];
};
