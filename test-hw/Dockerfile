FROM debian:13.4-slim@sha256:109e2c65005bf160609e4ba6acf7783752f8502ad218e298253428690b9eaa4b

ARG TARGETARCH
ARG ARDUINO_CLI_VERSION=1.5.1

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

RUN case "${TARGETARCH}" in \
      amd64) archive_arch=64bit; \
             archive_sha256=28a8e119c498a25607821c36cb2dc49e8463941b261a0d99091baa7bc692dd2b ;; \
      arm64) archive_arch=ARM64; \
             archive_sha256=1e69e077479f300614d4551334e0a33f08ee40b04315d83b8e7e0e94f0d0ee62 ;; \
      *) echo "Unsupported Docker architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
    && archive="arduino-cli_${ARDUINO_CLI_VERSION}_Linux_${archive_arch}.tar.gz" \
    && curl -fsSL \
       "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/${archive}" \
       -o "/tmp/${archive}" \
    && echo "${archive_sha256}  /tmp/${archive}" | sha256sum -c - \
    && tar -xzf "/tmp/${archive}" -C /usr/local/bin arduino-cli \
    && rm "/tmp/${archive}"

COPY arduino-cli.yaml /etc/arduino-cli.yaml

RUN arduino-cli --config-file /etc/arduino-cli.yaml core update-index \
    && arduino-cli --config-file /etc/arduino-cli.yaml core install esp32:esp32@3.3.11 \
    && arduino-cli --config-file /etc/arduino-cli.yaml lib install \
       "Etherkit JTEncode@1.3.1" "Etherkit Si5351@2.2.0" \
    && chmod -R a+rX /opt/arduino

WORKDIR /work
