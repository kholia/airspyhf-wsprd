#include <Arduino.h>
#include <JTEncode.h>
#include <Wire.h>
#include <esp_timer.h>
#include <si5351.h>

// ESP32-S3-Zero to Si5351 wiring.
constexpr int I2C_SDA_PIN = 1;
constexpr int I2C_SCL_PIN = 2;
constexpr uint8_t SI5351_I2C_ADDRESS = 0x60;

// This fixture drives XA from a 25 MHz TCXO, so no crystal load is required.
constexpr uint32_t SI5351_XTAL_FREQUENCY_HZ = 25000000U;
constexpr int32_t SI5351_CORRECTION = 0;

// 20m WSPR test carrier, 1520 Hz above the 14.095600 MHz dial frequency.
// Etherkit Si5351 frequencies are expressed in hundredths of a hertz.
constexpr uint64_t BASE_FREQUENCY_CENTIHZ = 14097120ULL * 100ULL;
constexpr uint32_t SYMBOL_DURATION_US = 682667U;

constexpr char TEST_CALLSIGN[] = "VU3CER";
constexpr char TEST_GRID[] = "MK68";
constexpr uint8_t TEST_POWER_DBM = 10;

Si5351 synthesizer;
JTEncode encoder;
uint8_t tones[WSPR_SYMBOL_COUNT];
char command_buffer[24];
size_t command_length;
bool radio_ready;

static uint64_t tone_frequency(uint8_t tone)
{
  // WSPR spacing is 375/256 Hz. In centihertz this is 9375/64.
  const uint64_t offset_centihz =
      (static_cast<uint64_t>(tone) * 9375ULL + 32ULL) / 64ULL;
  return BASE_FREQUENCY_CENTIHZ + offset_centihz;
}

static bool si5351_present()
{
  Wire.beginTransmission(SI5351_I2C_ADDRESS);
  return Wire.endTransmission() == 0;
}

static void transmit_wspr()
{
  Serial.println("TX START VU3CER MK68 10");
  Serial.flush();

  for (size_t symbol = 0; symbol < WSPR_SYMBOL_COUNT; symbol++) {
    const int64_t started_at = esp_timer_get_time();
    synthesizer.set_freq(tone_frequency(tones[symbol]), SI5351_CLK0);
    if (symbol == 0) {
      synthesizer.output_enable(SI5351_CLK0, 1);
    }
    while (esp_timer_get_time() - started_at < SYMBOL_DURATION_US) {
      // Busy waiting avoids scheduler and delay() timing jitter.
    }
  }

  synthesizer.output_enable(SI5351_CLK0, 0);
  Serial.println("TX DONE");
}

static void process_command()
{
  command_buffer[command_length] = '\0';
  if (strcmp(command_buffer, "*tx*") == 0) {
    if (radio_ready) {
      transmit_wspr();
    } else {
      Serial.println("ERROR RADIO NOT READY");
    }
  } else if (strcmp(command_buffer, "*ping*") == 0) {
    Serial.println("PONG");
  } else if (command_length != 0) {
    Serial.println("ERROR COMMAND");
  }
  command_length = 0;
}

void setup()
{
  Serial.begin(115200);
  const uint32_t serial_deadline = millis() + 3000U;
  while (!Serial && static_cast<int32_t>(millis() - serial_deadline) < 0) {
    delay(10);
  }

  Wire.setPins(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setTimeout(1000U);
  Wire.begin();
  Wire.setClock(100000U);

  if (!si5351_present()) {
    Serial.println("ERROR SI5351 NOT FOUND ON GPIO1/GPIO2");
    return;
  }
  if (!synthesizer.init(SI5351_CRYSTAL_LOAD_0PF,
                        SI5351_XTAL_FREQUENCY_HZ,
                        SI5351_CORRECTION)) {
    Serial.println("ERROR SI5351 INIT");
    return;
  }

  synthesizer.drive_strength(SI5351_CLK0, SI5351_DRIVE_2MA);
  synthesizer.set_freq(BASE_FREQUENCY_CENTIHZ, SI5351_CLK0);
  synthesizer.output_enable(SI5351_CLK0, 0);
  encoder.wspr_encode(TEST_CALLSIGN, TEST_GRID, TEST_POWER_DBM, tones);
  radio_ready = true;

  Serial.println("READY ESP32-S3-ZERO SI5351 GPIO1=SDA GPIO2=SCL");
  Serial.println("SEND *tx* AT AN EVEN UTC MINUTE");
}

void loop()
{
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\n' || character == '\r') {
      if (command_length != 0) {
        process_command();
      }
    } else if (command_length + 1U < sizeof(command_buffer)) {
      command_buffer[command_length++] = character;
    } else {
      command_length = 0;
      Serial.println("ERROR COMMAND TOO LONG");
    }
  }
  delay(1);
}
