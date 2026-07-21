// Host-side unit tests for the HD-rumble amplitude decode (procon_reports.h).
// Feeds canned 0x01 / 0x10 output-report payloads and asserts the decoded
// per-side amplitudes and the RUMBLE_MIN threshold crossing. Pure, dependency
// free -- no ESP-IDF needed (run with `pio test -e native`).

#include <unity.h>

#include "procon/procon_reports.h"

using procon::decodeRumbleAmplitude;
using procon::kRumbleMin;

// Neutral "no rumble" side payload.
static const uint8_t kNeutral[4] = {0x00, 0x01, 0x40, 0x40};

void setUp(void) {}
void tearDown(void) {}

void test_neutral_is_zero(void) {
  TEST_ASSERT_EQUAL_UINT8(0, decodeRumbleAmplitude(kNeutral));
}

void test_high_band_amplitude(void) {
  // High-band amplitude in byte 1 (bits 7..1); freq bit 0 is ignored.
  const uint8_t strong[4] = {0x00, 0xFF, 0x40, 0x40};
  TEST_ASSERT_EQUAL_UINT8(0xFE, decodeRumbleAmplitude(strong));
  // The low freq bit alone must not register as amplitude.
  const uint8_t freqOnly[4] = {0xAA, 0x01, 0x40, 0x40};
  TEST_ASSERT_EQUAL_UINT8(0, decodeRumbleAmplitude(freqOnly));
}

void test_low_band_amplitude(void) {
  // Low-band amplitude shows up as byte 3's deviation from the 0x40 neutral,
  // scaled x2 and clamped to 255.
  const uint8_t lo[4] = {0x00, 0x01, 0x40, 0x60};  // +0x20 -> 0x40 (64)
  TEST_ASSERT_EQUAL_UINT8(0x40, decodeRumbleAmplitude(lo));
  const uint8_t loMax[4] = {0x00, 0x01, 0x40, 0xFF};  // clamps to 255
  TEST_ASSERT_EQUAL_UINT8(0xFF, decodeRumbleAmplitude(loMax));
}

void test_threshold_crossing(void) {
  // Just below and at/above the death threshold.
  const uint8_t below[4] = {0x00, (uint8_t)((kRumbleMin - 2) & 0xFE), 0x40, 0x40};
  const uint8_t at[4] = {0x00, (uint8_t)(kRumbleMin & 0xFE), 0x40, 0x40};
  TEST_ASSERT_TRUE(decodeRumbleAmplitude(below) < kRumbleMin);
  TEST_ASSERT_TRUE(decodeRumbleAmplitude(at) >= kRumbleMin);
}

void test_full_eight_byte_payload(void) {
  // A whole 0x01/0x10 rumble payload: left neutral, right strong.
  const uint8_t payload[8] = {0x00, 0x01, 0x40, 0x40,   // left  (RUMBLE_A)
                              0x00, 0xFE, 0x40, 0x40};  // right (RUMBLE_B)
  TEST_ASSERT_EQUAL_UINT8(0, decodeRumbleAmplitude(payload));
  TEST_ASSERT_EQUAL_UINT8(0xFE, decodeRumbleAmplitude(payload + 4));
  TEST_ASSERT_TRUE(decodeRumbleAmplitude(payload + 4) >= kRumbleMin);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_neutral_is_zero);
  RUN_TEST(test_high_band_amplitude);
  RUN_TEST(test_low_band_amplitude);
  RUN_TEST(test_threshold_crossing);
  RUN_TEST(test_full_eight_byte_payload);
  return UNITY_END();
}
