# Unattended-operation checklist

The supplied systemd unit covers the essential baseline: an unprivileged
service account, filesystem hardening, boot startup, clean shutdown, and
unlimited 15-second restarts after receiver or USB failures. All frame,
decoder, hash-table, timing, and journal data lives in RAM below `/run`; service
swap and core dumps are disabled. The receiver therefore makes no steady-state
writes to the microSD card.

For a dependable field installation:

- Use a Raspberry Pi 3 or newer, a reputable correctly rated PSU, and a short,
  shielded USB cable. The HF+ load is modest and does not require a powered USB
  hub. Avoid early Pi boards for unattended receivers.
- Enable NTP and verify the clock after every boot. WSPR timing is part of the
  signal, so a running process with a bad clock is not healthy.
- Keep the Airspy away from switch-mode supplies and the Pi; use ferrites and a
  common RF ground where appropriate.
- Run a multi-day soak test, then test loss and recovery of mains power,
  networking, time service, and USB separately.
- Watch `journalctl -u airspyhf-wsprd` for stream stops, dropped samples,
  decoder failures, and upload failures. The supplied journald configuration
  keeps these logs in a bounded 16 MiB RAM buffer.
- For an absolute whole-appliance guarantee, also configure the operating
  system root filesystem read-only and disable any non-journald services that
  write logs or state. The receiver service cannot control unrelated programs.

Implemented software safeguards include:

- A 4096-spot RAM-only WSPRnet queue. Uploading runs outside the decoder thread,
  checks HTTP results, and retries transient errors with bounded exponential
  backoff.
- A bounded RAM-only PSK Reporter queue for FT8. It batches validated callsigns
  into IPFIX/UDP packets, suppresses five-minute duplicates, sends templates at
  the required cadence, and never blocks capture or decoding.
- A 32-block RAM-only IQ queue. The Airspy callback only copies samples, while a
  dedicated DSP thread performs decimation. The 192 ksample/s default reduces
  USB and host load without changing the decoder's 375 sample/s input.
- A Linux kernel clock-health gate. Capture pauses while NTP marks the clock
  unsynchronized, and unsynchronized spots are never reported.
- A dependency-free systemd readiness and watchdog heartbeat. A hung process is
  killed and restarted automatically.
- Optional adaptive hopping uses one bounded regional aggregate query to
  wspr.live per hour for WSPR. FT8 ranks PSK Reporter's active monitors by
  band at startup, with its best-frequency service as a small fallback. The
  initial result is activated before capture using a bounded wait. Later
  queries run outside capture, state stays in RAM, and plans change only at
  mode boundaries: two minutes for FT8 and ten minutes for WSPR. Any failure
  retains the local schedule.
- Strict callsign and Maidenhead validation with predictable normalization.
- Exact WSPR and FT8 decoder fixtures, reporting protocol tests, strict compiler
  warnings, and shell syntax checks in `make test`.
- ARM64 Raspberry Pi installations add the verified WSJT-X Improved `jt9`
  executable as a second FT8 decoder. It runs concurrently with `ft8_lib` using
  `--ft8 -M -C 2 -m 2`; results are deduplicated before PSK Reporter queuing.
  All per-frame `jt9` files and writable decoder state live under `/run`.

Remaining qualification work is Linux ARM CI, sanitizer runs, and automated
multi-day USB, network, and power-cycle soak testing.
