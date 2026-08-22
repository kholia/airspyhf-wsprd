#!/bin/sh
set -eu

unit=systemd/airspyhf-wsprd.service

grep -q '^RuntimeDirectory=airspyhf-wsprd$' "$unit"
grep -q '^Environment=AIRSPYHF_WSPRD_RUNTIME_DIR=/run/airspyhf-wsprd$' "$unit"
grep -q '^MemorySwapMax=0$' "$unit"
grep -q '^LimitCORE=0$' "$unit"
grep -q '^WatchdogSec=' "$unit"
grep -q '^TimeoutStartSec=180s$' "$unit"
grep -q '^StartLimitIntervalSec=0$' "$unit"
grep -q '^Restart=on-failure$' "$unit"
grep -q '^RestartSec=15s$' "$unit"
grep -q '^Environment=HEALTH_BIND=0.0.0.0$' "$unit"
grep -q '^Environment=HEALTH_PORT=8080$' "$unit"
grep -q '^Environment=MODE=WSPR$' "$unit"
grep -q -- '--mode ${MODE}' "$unit"
grep -q '\$HOPPING' "$unit"
grep -q -- '-e ${HEALTH_BIND} -P ${HEALTH_PORT}' "$unit"
grep -q '^HOPPING=$' systemd/airspyhf-wsprd.default
grep -q '^MODE=WSPR$' systemd/airspyhf-wsprd.default
grep -q '^HEALTH_BIND=0.0.0.0$' systemd/airspyhf-wsprd.default
grep -q '^HEALTH_PORT=8080$' systemd/airspyhf-wsprd.default
grep -q '^prompt_mode()' install.sh
grep -q '^prompt_mode$' install.sh
grep -q 'prompt_band_hopping' install.sh
grep -q 'reporting_network=WSPRnet' install.sh
grep -q "reporting_network='PSK Reporter'" install.sh
grep -q "hopping_advisor='PSK Reporter active-monitor counts'" install.sh
grep -q "hopping_interval='2-minute'" install.sh
grep -q "printf 'HOPPING=%s" install.sh
grep -q "printf 'HEALTH_BIND=0.0.0.0" install.sh
grep -q "printf 'MODE=%s" install.sh
grep -q 'installer 1.0' install.sh
grep -q 'dpkg --fsys-tarfile' install.sh
grep -q 'tar -xO ./usr/bin/jt9' install.sh
grep -q 'libgfortran5' install.sh
grep -q 'libqt5core5t64' install.sh
grep -q 'AIRSPYHF_WSPRD_JT9=%s' install.sh
grep -q 'Reusing the verified installed Raspberry Pi jt9 decoder' install.sh
grep -q 'a508a58c3990bf21ab98c1fe477fc9cff4e3e9cca4feefa548d0942c0f848506' install.sh
if grep -q '^MemoryDenyWriteExecute=true$' "$unit"; then
    echo "MemoryDenyWriteExecute blocks the jt9 OpenMP worker threads" >&2
    exit 1
fi
if grep -q '^RuntimeDirectoryPreserve=' "$unit"; then
    echo "systemd must clear stale RAM decoder files between restarts" >&2
    exit 1
fi
if grep -q '^StateDirectory=' "$unit"; then
    echo "systemd service must not use persistent StateDirectory" >&2
    exit 1
fi
grep -q '^Storage=volatile$' systemd/airspyhf-wsprd-volatile-journal.conf

echo "service: unlimited SDR retries, RAM runtime, no swap/core dumps, watchdog verified"
