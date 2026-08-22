CC ?= cc
AIRSPYHF_CFLAGS := $(shell pkg-config --cflags libairspyhf 2>/dev/null)
AIRSPYHF_LIBS := $(shell pkg-config --libs libairspyhf 2>/dev/null || echo -lairspyhf)
WSPRD_DIR ?= core
PREFIX ?= /usr/local
HEALTH_URL ?= http://rpi.local:8080/health
RX_CALL ?= VU3CER
WSPR_LAT ?= 18.520833
WSPR_LON ?= 73.958333
WSPR_RADIUS_KM ?= 1500
WSPR_LOOKBACK_HOURS ?= 2
WSPR_DB_BAND ?= 7

CFLAGS ?= -std=gnu11
CORE_CFLAGS ?= -funroll-loops -std=gnu11
STRICT_CFLAGS = -O3 -Wall -Wextra -Werror -Wformat=2 -Wformat-security \
	-Werror=implicit-function-declaration -Werror=return-type \
	-D_FORTIFY_SOURCE=2 -fstack-protector-strong -fno-common \
	-fno-fast-math -fno-strict-aliasing
override CFLAGS += $(STRICT_CFLAGS) $(AIRSPYHF_CFLAGS) -Ift8_lib
override CORE_CFLAGS += $(STRICT_CFLAGS)
LDFLAGS ?=
LIBS = $(AIRSPYHF_LIBS) -lpthread -lcurl -lm

TARGET = airspyhf-wsprd
CORE_TARGET = airspyhf-wsprd-core
OBJS = airspyhf_wsprd.o band_advisor.o bands.o clock_health.o decoder_bridge.o decimator.o ft8_decoder.o ft8_resampler.o health_server.o jt9_decoder.o psk_reporter.o reporter.o sample_queue.o service_notify.o station.o
CORE_SRCS = $(addprefix $(WSPRD_DIR)/,wsprd.c wsprd_utils.c wsprsim_utils.c tab.c fano.c jelinek.c nhash.c pffft.c)
FT8_LIB_SRCS = $(wildcard ft8_lib/ft8/*.c ft8_lib/fft/*.c) ft8_lib/common/monitor.c
FT8_LIB_OBJS = $(FT8_LIB_SRCS:.c=.o)
TEST_TARGETS = tests/test_band_advisor tests/test_bands tests/test_decimator tests/test_decoder_bridge tests/test_ft8_decoder tests/test_ft8_resampler tests/test_health_server tests/test_jt9_decoder tests/test_psk_reporter tests/test_reporter tests/test_sample_queue tests/test_station

all: $(TARGET)

%.o: %.c
	${CC} ${CFLAGS} -c $< -o $@

$(FT8_LIB_OBJS): %.o: %.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -Wno-array-parameter -Wno-unused-but-set-variable \
		-Wno-unused-variable -Wno-unused-function \
		-Ift8_lib -include ft8_silence.h -c $< -o $@

ft8-lib-check: $(FT8_LIB_OBJS)

$(TARGET): $(OBJS) $(FT8_LIB_OBJS) $(CORE_TARGET)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(FT8_LIB_OBJS) $(LIBS)

$(CORE_TARGET): $(CORE_SRCS) osd_stub.c
	$(CC) $(CORE_CFLAGS) -I$(WSPRD_DIR) -o $@ $(CORE_SRCS) osd_stub.c -lm

tests/test_decimator: tests/test_decimator.c decimator.c decimator.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_decimator.c decimator.c -lm

tests/test_bands: tests/test_bands.c bands.c bands.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_bands.c bands.c

tests/test_band_advisor: tests/test_band_advisor.c band_advisor.c band_advisor.h bands.c bands.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_band_advisor.c band_advisor.c bands.c -lcurl -lpthread -lm

tests/test_decoder_bridge: tests/test_decoder_bridge.c decoder_bridge.c decoder_bridge.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_decoder_bridge.c decoder_bridge.c

tests/test_ft8_decoder: tests/test_ft8_decoder.c ft8_decoder.c ft8_decoder.h $(FT8_LIB_OBJS)
	$(CC) $(CFLAGS) -I. -Ift8_lib -o $@ tests/test_ft8_decoder.c \
		ft8_decoder.c $(FT8_LIB_OBJS) -lm

tests/test_ft8_resampler: tests/test_ft8_resampler.c ft8_resampler.c ft8_resampler.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_ft8_resampler.c ft8_resampler.c -lm

tests/test_health_server: tests/test_health_server.c health_server.c health_server.h bands.c bands.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_health_server.c health_server.c bands.c -lpthread

tests/test_jt9_decoder: tests/test_jt9_decoder.c jt9_decoder.c jt9_decoder.h station.c station.h ft8_decoder.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_jt9_decoder.c jt9_decoder.c station.c -lm

tests/test_psk_reporter: tests/test_psk_reporter.c psk_reporter.c psk_reporter.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_psk_reporter.c psk_reporter.c -lpthread

tests/test_reporter: tests/test_reporter.c reporter.c reporter.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_reporter.c reporter.c -lcurl -lpthread

tests/test_sample_queue: tests/test_sample_queue.c sample_queue.c sample_queue.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_sample_queue.c sample_queue.c -lpthread

tests/test_station: tests/test_station.c station.c station.h
	$(CC) $(CFLAGS) -I. -o $@ tests/test_station.c station.c

test: $(TARGET) $(TEST_TARGETS)
	sh -n install.sh
	sh -n scripts/install-systemd.sh
	sh -n scripts/test-ubuntu.sh
	sh -n scripts/update-checkout.sh
	sh tests/test_installer_update.sh
	sh tests/test_modes.sh
	sh tests/test_service.sh
	./tests/test_band_advisor
	./tests/test_bands
	./tests/test_decimator
	./tests/test_ft8_decoder
	./tests/test_ft8_resampler
	./tests/test_health_server
	./tests/test_jt9_decoder
	./tests/test_psk_reporter
	./tests/test_reporter
	./tests/test_sample_queue
	./tests/test_station
	./tests/test_wsprd.sh

test-wav: tests/test_decoder_bridge $(CORE_TARGET)
	./tests/test_wsprd.sh 150426_0918.wav

test-ft8: tests/test_ft8_decoder tests/test_ft8_resampler tests/test_jt9_decoder tests/test_psk_reporter
	./tests/test_ft8_decoder
	./tests/test_ft8_resampler
	./tests/test_jt9_decoder
	./tests/test_psk_reporter

test-ubuntu: scripts/Dockerfile.ubuntu-test scripts/test-ubuntu.sh
	./scripts/test-ubuntu.sh

diagnose-health:
	@printf '\nReceiver health\n'
	@curl -sS --max-time 10 "$(HEALTH_URL)"

diagnose-propagation:
	@printf '\nRegional activity by band\n'
	@curl -fsS --max-time 20 --get \
		--data-urlencode "query=SELECT band, count() AS spots, uniqExact(tx_sign) AS tx, uniqExact(rx_sign) AS rx, max(time) AS latest FROM wspr.rx WHERE time > subtractHours(now(), $(WSPR_LOOKBACK_HOURS)) AND code = 1 AND greatCircleDistance(rx_lon, rx_lat, $(WSPR_LON), $(WSPR_LAT)) <= $(WSPR_RADIUS_KM) * 1000 AND band IN (3,7,10,14,18,21,24,28) GROUP BY band ORDER BY tx DESC FORMAT PrettyCompact" \
		https://db1.wspr.live/

diagnose-band:
	@printf '\nRegional detail for database band $(WSPR_DB_BAND)\n'
	@curl -fsS --max-time 20 --get \
		--data-urlencode "query=SELECT rx_sign, rx_loc, tx_sign, tx_loc, count() AS spots, round(avg(snr), 1) AS avg_snr, max(time) AS latest FROM wspr.rx WHERE time > subtractHours(now(), $(WSPR_LOOKBACK_HOURS)) AND code = 1 AND greatCircleDistance(rx_lon, rx_lat, $(WSPR_LON), $(WSPR_LAT)) <= $(WSPR_RADIUS_KM) * 1000 AND band = $(WSPR_DB_BAND) GROUP BY rx_sign, rx_loc, tx_sign, tx_loc ORDER BY latest DESC FORMAT PrettyCompact" \
		https://db1.wspr.live/

diagnose-station:
	@printf '\nLatest reports uploaded by $(RX_CALL)\n'
	@curl -fsS --max-time 20 --get \
		--data-urlencode "query=SELECT time, band, tx_sign, tx_loc, snr, frequency FROM wspr.rx WHERE time > subtractDays(now(), 1) AND code = 1 AND upperUTF8(rx_sign) = upperUTF8('$(RX_CALL)') ORDER BY time DESC LIMIT 20 FORMAT PrettyCompact" \
		https://db1.wspr.live/

diagnose-global:
	@printf '\nGlobal 30-minute control\n'
	@curl -fsS --max-time 20 --get \
		--data-urlencode "query=SELECT count() AS spots, uniqExact(tx_sign) AS tx, uniqExact(rx_sign) AS rx, max(time) AS latest FROM wspr.rx WHERE time > subtractMinutes(now(), 30) AND code = 1 AND band IN (3,7,10,14,18,21,24,28) FORMAT PrettyCompact" \
		https://db1.wspr.live/

diagnose-live: diagnose-health diagnose-propagation diagnose-band diagnose-station diagnose-global

esp32-build:
	./test-hw/build-local.sh

esp32-build-docker:
	./test-hw/build.sh

esp32-check-port:
	@test -n "$(PORT)" || (echo "Usage: make esp32-flash PORT=/dev/cu.usbmodemXXXX" >&2; exit 1)

esp32-flash: esp32-check-port esp32-build
	./test-hw/flash.sh "$(PORT)"

esp32-ping:
	./test-hw/command.py ping $(if $(PORT),--port "$(PORT)",)

esp32-arm:
	./test-hw/command.py arm $(if $(PORT),--port "$(PORT)",)

esp32-tx-now:
	./test-hw/command.py tx-now $(if $(PORT),--port "$(PORT)",)

hw-build: esp32-build

hw-flash: esp32-flash

hw-ping: esp32-ping

hw-arm: esp32-arm

hw-tx-now: esp32-tx-now

test-hw: $(TARGET)
	./test-hw/run.py $(if $(PORT),--port "$(PORT)",)

sync-wsprd-core:
	./scripts/sync-wsprd-core.sh

sync-ft8-lib:
	./scripts/sync-ft8-lib.sh

install: $(TARGET) $(CORE_TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(CORE_TARGET) $(DESTDIR)$(PREFIX)/bin

clean:
	rm -f *.o $(FT8_LIB_OBJS) $(TARGET) $(CORE_TARGET) wsprd-core $(TEST_TARGETS) wspr_wisdom.dat hashtable.txt

.PHONY: all clean diagnose-band diagnose-global diagnose-health diagnose-live \
	diagnose-propagation diagnose-station esp32-arm esp32-build esp32-build-docker esp32-check-port ft8-lib-check \
	esp32-flash esp32-ping esp32-tx-now hw-arm hw-build hw-flash hw-ping hw-tx-now install \
	sync-ft8-lib sync-wsprd-core test test-ft8 test-hw test-ubuntu test-wav
