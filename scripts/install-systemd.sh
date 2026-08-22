#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root: sudo ./scripts/install-systemd.sh" >&2
    exit 1
fi

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if ! command -v systemctl >/dev/null 2>&1; then
    echo "systemd was not found; this installer is for Linux systems using systemd." >&2
    exit 1
fi

if ! getent group plugdev >/dev/null 2>&1; then
    groupadd --system plugdev
fi
if ! getent group airspyhf-wsprd >/dev/null 2>&1; then
    groupadd --system airspyhf-wsprd
fi
if ! id airspyhf-wsprd >/dev/null 2>&1; then
    useradd --system \
        --gid airspyhf-wsprd \
        --groups plugdev \
        --home-dir /nonexistent \
        --no-create-home \
        --shell /usr/sbin/nologin \
        airspyhf-wsprd
else
    usermod --append --groups plugdev airspyhf-wsprd
fi

make -C "$project_dir" install PREFIX=/usr/local
install -d -m 0755 /etc/default /etc/systemd/system /etc/systemd/journald.conf.d
install -m 0644 "$project_dir/systemd/airspyhf-wsprd.service" \
    /etc/systemd/system/airspyhf-wsprd.service
install -m 0644 "$project_dir/systemd/airspyhf-wsprd-volatile-journal.conf" \
    /etc/systemd/journald.conf.d/airspyhf-wsprd-volatile.conf
if [ ! -e /etc/default/airspyhf-wsprd ]; then
    install -m 0640 -o root -g airspyhf-wsprd \
        "$project_dir/systemd/airspyhf-wsprd.default" \
        /etc/default/airspyhf-wsprd
fi

systemctl daemon-reload
systemctl restart systemd-journald

echo
echo "Installed, but not enabled (reporting is safely disabled)."
echo "1. Edit /etc/default/airspyhf-wsprd"
echo "2. Test:   systemctl start airspyhf-wsprd"
echo "3. Check:  journalctl -u airspyhf-wsprd -f"
echo "4. Enable: systemctl enable airspyhf-wsprd"
