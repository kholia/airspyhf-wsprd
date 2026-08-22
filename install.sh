#!/bin/sh
set -eu

repository=${AIRSPYHF_WSPRD_REPOSITORY:-https://github.com/kholia/airspyhf-wsprd.git}
install_directory=${AIRSPYHF_WSPRD_INSTALL_DIR:-/opt/airspyhf-wsprd}
jt9_url='https://sourceforge.net/projects/wsjt-x-improved/files/WSJT-X_v3.2.0/Raspberry%20Pi/wsjtx-3.2.0_improved_PLUS_260818_Rpi_trixie_arm64.deb/download'
jt9_deb_sha256='a508a58c3990bf21ab98c1fe477fc9cff4e3e9cca4feefa548d0942c0f848506'
jt9_binary_sha256='c8ecd88d4e94d0f582c6f4ccd58d5debd394b98c41646dd71e6a06fa487ed934'
jt9_install_path=/usr/local/libexec/airspyhf-wsprd-jt9
build_directory=
jt9_ram_directory=

cleanup() {
    if [ -n "$build_directory" ]; then
        rm -rf "$build_directory"
    fi
    if [ -n "$jt9_ram_directory" ]; then
        rm -rf "$jt9_ram_directory"
    fi
}
trap cleanup EXIT HUP INT TERM

echo "airspyhf-wsprd installer 1.0"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo, as shown in the README." >&2
    exit 1
fi
if [ ! -r /dev/tty ]; then
    echo "An interactive terminal is required for receiver setup." >&2
    exit 1
fi
if ! command -v apt-get >/dev/null 2>&1; then
    echo "This automatic installer supports Debian, Ubuntu, and Raspberry Pi OS." >&2
    exit 1
fi

prompt_mode() {
    while :; do
        printf "Receiver mode [WSPR/FT8] (default WSPR): " >/dev/tty
        IFS= read -r entered_mode </dev/tty
        selected_mode=$(printf '%s' "$entered_mode" | tr '[:lower:]' '[:upper:]')
        case "$selected_mode" in
            ''|WSPR )
                mode=WSPR
                reporting_network=WSPRnet
                hopping_advisor=wspr.live
                hopping_interval='10-minute'
                return
                ;;
            FT8 )
                mode=FT8
                reporting_network='PSK Reporter'
                hopping_advisor='PSK Reporter active-monitor counts'
                hopping_interval='2-minute'
                return
                ;;
            * ) echo "Please enter WSPR or FT8." >/dev/tty ;;
        esac
    done
}

prompt_station() {
    while :; do
        printf "Station callsign: " >/dev/tty
        IFS= read -r entered_call </dev/tty
        callsign=$(printf '%s' "$entered_call" | tr '[:lower:]' '[:upper:]')
        call_length=${#callsign}
        case "$callsign" in
            ''|*[!A-Z0-9/]*|/*|*/|*//* ) valid_call=false ;;
            * ) valid_call=true ;;
        esac
        case "$callsign" in *[A-Z]* ) : ;; * ) valid_call=false ;; esac
        case "$callsign" in *[0-9]* ) : ;; * ) valid_call=false ;; esac
        if [ "$call_length" -lt 3 ] || [ "$call_length" -gt 12 ]; then
            valid_call=false
        fi
        if [ "$valid_call" != true ]; then
            echo "Use a valid 3-12 character amateur callsign." >/dev/tty
            continue
        fi

        printf "Maidenhead grid (4 or 6 characters): " >/dev/tty
        IFS= read -r entered_grid </dev/tty
        grid_upper=$(printf '%s' "$entered_grid" | tr '[:lower:]' '[:upper:]')
        if ! printf '%s\n' "$grid_upper" | grep -Eq '^[A-R]{2}[0-9]{2}([A-X]{2})?$'; then
            echo "Use a valid grid such as IO91 or IO91wm." >/dev/tty
            continue
        fi
        if [ "${#grid_upper}" -eq 6 ]; then
            grid_prefix=$(printf '%s' "$grid_upper" | cut -c 1-4)
            grid_suffix=$(printf '%s' "$grid_upper" | cut -c 5-6 | tr '[:upper:]' '[:lower:]')
            grid=${grid_prefix}${grid_suffix}
        else
            grid=$grid_upper
        fi

        printf "Use callsign %s and grid %s in %s mode with %s reporting enabled? [y/N] " \
            "$callsign" "$grid" "$mode" "$reporting_network" >/dev/tty
        IFS= read -r confirmation </dev/tty
        case "$confirmation" in
            y|Y|yes|YES|Yes ) return ;;
            * ) echo "Let us try again." >/dev/tty ;;
        esac
    done
}

prompt_band_hopping() {
    while :; do
        printf "Enable adaptive %s band hopping using %s? [y/N] " \
            "$hopping_interval" "$hopping_advisor" >/dev/tty
        IFS= read -r confirmation </dev/tty
        case "$confirmation" in
            y|Y|yes|YES|Yes ) hopping=-Bauto; return ;;
            ''|n|N|no|NO|No ) hopping=; return ;;
            * ) echo "Please answer y or n." >/dev/tty ;;
        esac
    done
}

prompt_mode
prompt_station
prompt_band_hopping

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y build-essential ca-certificates cmake curl git \
    libcurl4-openssl-dev libusb-1.0-0-dev pkg-config

raspberry_pi=false
for model_path in /proc/device-tree/model /sys/firmware/devicetree/base/model; do
    if [ -r "$model_path" ] && grep -aqi 'Raspberry Pi' "$model_path"; then
        raspberry_pi=true
        break
    fi
done

jt9_configured_path=
if [ "$raspberry_pi" = true ]; then
    case "$(uname -m)" in
        aarch64|arm64 )
            if [ ! -d /dev/shm ] || [ ! -w /dev/shm ]; then
                echo "Raspberry Pi jt9 setup requires writable RAM at /dev/shm." >&2
                exit 1
            fi
            apt-get install -y libfftw3-single3 libgfortran5 libgomp1 \
                libqt5core5t64
            installed_sha256=
            if [ -x "$jt9_install_path" ]; then
                installed_sha256=$(sha256sum "$jt9_install_path" | \
                    cut -d ' ' -f 1)
            fi
            if [ "$installed_sha256" = "$jt9_binary_sha256" ]; then
                echo "Reusing the verified installed Raspberry Pi jt9 decoder."
            else
                jt9_ram_directory=$(mktemp -d /dev/shm/airspyhf-jt9.XXXXXX)
                curl -fL --retry 3 --output "$jt9_ram_directory/wsjtx.deb" \
                    "$jt9_url"
                downloaded_sha256=$(sha256sum "$jt9_ram_directory/wsjtx.deb" | \
                    cut -d ' ' -f 1)
                if [ "$downloaded_sha256" != "$jt9_deb_sha256" ]; then
                    echo "Downloaded WSJT-X package failed its SHA-256 check." >&2
                    exit 1
                fi
                dpkg --fsys-tarfile "$jt9_ram_directory/wsjtx.deb" | \
                    tar -xO ./usr/bin/jt9 >"$jt9_ram_directory/jt9"
                extracted_sha256=$(sha256sum "$jt9_ram_directory/jt9" | \
                    cut -d ' ' -f 1)
                if [ "$extracted_sha256" != "$jt9_binary_sha256" ]; then
                    echo "Extracted jt9 failed its SHA-256 check." >&2
                    exit 1
                fi
                chmod 0755 "$jt9_ram_directory/jt9"
                install -d -m 0755 /usr/local/libexec
                install -m 0755 "$jt9_ram_directory/jt9" "$jt9_install_path"
                echo "Installed the verified Raspberry Pi jt9 parallel FT8 decoder."
            fi
            if ldd "$jt9_install_path" | grep -q 'not found'; then
                echo "Installed jt9 has unresolved runtime libraries." >&2
                exit 1
            fi
            jt9_configured_path=$jt9_install_path
            ;;
        * )
            echo "Raspberry Pi is not running ARM64; using ft8_lib only." >&2
            ;;
    esac
fi

if ! pkg-config --exists libairspyhf; then
    if ! apt-get install -y libairspyhf-dev; then
        build_directory=$(mktemp -d /tmp/airspyhf-build.XXXXXX)
        git clone --depth 1 https://github.com/airspy/airspyhf.git \
            "$build_directory/airspyhf"
        cmake -S "$build_directory/airspyhf" \
            -B "$build_directory/airspyhf/build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DINSTALL_UDEV_RULES=ON
        cmake --build "$build_directory/airspyhf/build" --parallel
        cmake --install "$build_directory/airspyhf/build"
        ldconfig
    fi
fi

if [ -d "$install_directory/.git" ]; then
    origin=$(git -C "$install_directory" remote get-url origin 2>/dev/null || true)
    if [ "$origin" != "$repository" ]; then
        echo "$install_directory has unexpected origin '$origin'." >&2
        echo "Expected '$repository'; refusing to overwrite this checkout." >&2
        exit 1
    fi
    git -C "$install_directory" fetch --depth 1 origin master
    for required_path in Makefile airspyhf_wsprd.c scripts/install-systemd.sh; do
        if ! git -C "$install_directory" cat-file -e "FETCH_HEAD:$required_path"; then
            echo "Fetched revision is not an airspyhf-wsprd source tree." >&2
            exit 1
        fi
    done
    git -C "$install_directory" reset --hard FETCH_HEAD
elif [ -e "$install_directory" ]; then
    echo "$install_directory exists but is not an airspyhf-wsprd Git checkout." >&2
    exit 1
else
    git clone --depth 1 "$repository" "$install_directory"
fi

make -C "$install_directory" clean all test
"$install_directory/scripts/install-systemd.sh"

configuration=/etc/default/airspyhf-wsprd
configuration_new=${configuration}.new
umask 027
{
    printf 'MODE=%s\n' "$mode"
    printf 'BAND=20m\n'
    printf 'CALLSIGN=%s\n' "$callsign"
    printf 'GRID=%s\n' "$grid"
    printf 'REPORTING=\n'
    printf 'HOPPING=%s\n' "$hopping"
    printf 'HEALTH_BIND=0.0.0.0\n'
    printf 'HEALTH_PORT=8080\n'
    printf 'AIRSPYHF_WSPRD_JT9=%s\n' "$jt9_configured_path"
    printf 'EXTRA_ARGS=\n'
} >"$configuration_new"
chown root:airspyhf-wsprd "$configuration_new"
chmod 0640 "$configuration_new"
mv "$configuration_new" "$configuration"

systemctl enable airspyhf-wsprd
if ! systemctl restart airspyhf-wsprd; then
    echo "The service has not started yet; the Airspy HF+ may be disconnected." >&2
fi

echo
if [ -n "$hopping" ]; then
    echo "airspyhf-wsprd is installed for $callsign at $grid in $mode mode with adaptive band hopping."
else
    echo "airspyhf-wsprd is installed for $callsign at $grid in $mode mode on 20m."
fi
echo "If the SDR is absent, systemd retries every 15 seconds until it is available."
echo "Status: systemctl status airspyhf-wsprd"
echo "RAM logs: journalctl -u airspyhf-wsprd -f"
echo "Health: curl http://127.0.0.1:8080/health"
