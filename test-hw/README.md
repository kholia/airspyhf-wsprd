# Hardware decode test

This fixture proves the complete path from an ESP32-S3-Zero and Si5351 through
the Airspy HF+ and `airspyhf-wsprd` decoder. The generator stays RF-silent until
the host sends the exact `*tx*` command over USB serial at an even UTC minute.

Wiring:

```text
ESP32-S3-Zero GP1  -> Si5351 SDA
ESP32-S3-Zero GP2  -> Si5351 SCL
ESP32-S3-Zero 3V3  -> Si5351 VCC
ESP32-S3-Zero GND  -> Si5351 GND
Si5351 CLK0        -> shielded attenuator or very weak near-field coupling
```

Do not connect CLK0 directly to the HF+ antenna input and do not attach an
antenna. Use shielding and enough attenuation to protect the receiver and avoid
radiating the test transmission.

Build with the installed Arduino CLI:

```sh
make esp32-build
```

For a reproducible Docker toolchain, run `make esp32-build-docker`. The image
pins Arduino CLI 1.5.1, ESP32 core 3.3.11, Etherkit JTEncode 1.3.1, Etherkit
Si5351 2.2.0, and the base image digest.

Compile and flash with the host Arduino CLI:

```sh
make esp32-flash PORT=/dev/cu.usbmodemXXXX
```

Linux commonly uses `PORT=/dev/ttyACM0`. Then run the approximately three to
five minute end-to-end test:

```sh
make test-hw PORT=/dev/cu.usbmodemXXXX
```

Manual generator controls are also available:

```sh
make esp32-ping PORT=/dev/cu.usbmodemXXXX
make esp32-arm PORT=/dev/cu.usbmodemXXXX
make esp32-tx-now PORT=/dev/cu.usbmodemXXXX
```

`esp32-arm` waits for the next even UTC minute before sending `*tx*`. The
`esp32-tx-now` target transmits immediately and is intended only for bench
checks. The shorter `hw-*` names remain aliases.

The host verifies the serial link, starts `airspyhf-wsprd -n`, waits for the
next even UTC boundary, sends `*tx*`, and requires this real RF decode:

```text
VU3CER MK68 10
```

The Si5351 fixture is configured for a 25 MHz external TCXO with the crystal
load disabled (`0 pF`), minimum 2 mA drive, and 14.097120 MHz on 20 metres.
