#!/bin/sh
set -eu

check_output() {
    expected=$1
    shift
    output=$({ "$@"; } 2>&1 || true)
    printf '%s\n' "$output" | grep -q "$expected"
}

check_output 'Mode must be WSPR or FT8' \
    ./airspyhf-wsprd --mode invalid -b 20m -c VU3CER -g MK68xm
check_output 'FT8 sample rate 256000 must be exactly divisible by 12000 Hz' \
    ./airspyhf-wsprd -b 20m -c VU3CER -g MK68xm -r 256k --mode FT8
check_output 'FT8 sample rate 256000 must be exactly divisible by 12000 Hz' \
    ./airspyhf-wsprd --mode FT8 -b 20m -c VU3CER -g MK68xm -r 256k
check_output 'FT8 sample rate 256000 must be exactly divisible by 12000 Hz' \
    ./airspyhf-wsprd --mode FT8 -B auto -c VU3CER -g MK68xm -r 256k
check_output 'FT8 sample rate 256000 must be exactly divisible by 12000 Hz' \
    ./airspyhf-wsprd --mode FT8 -b 20m -B auto -c VU3CER -g MK68xm -r 256k
check_output 'FT8 sample rate 256000 must be exactly divisible by 12000 Hz' \
    ./airspyhf-wsprd -B 80m,40m,20m --mode FT8 -c VU3CER -g MK68xm -r 256k
check_output "Invalid FT8 hopping list '20m,6m'" \
    ./airspyhf-wsprd --mode FT8 -B 20m,6m -c VU3CER -g MK68xm
check_output 'WSPR sample rate 256000 must be exactly divisible by 375 Hz' \
    ./airspyhf-wsprd -b 20m -c VU3CER -g MK68xm -r 256k

echo 'modes: WSPR compatibility and order-independent FT8 selection verified'
