// Opto 1 test: drives ONLY GPIO13 (relay/supply-switch optocoupler).
// GPIO14 and GPIO26 (half-bridge gate commands) are held LOW the whole time.
//
// Build/upload with:  platformio run -e opto13_test -t upload
// Serial 115200:
//   on   GPIO13 HIGH, stays ON until 'off' (no timeout, by user request)
//   off  GPIO13 LOW
#include <Arduino.h>

constexpr uint8_t kOptoPin = 13;
constexpr uint8_t kSwitchPinA = 14;
constexpr uint8_t kSwitchPinB = 26;

void setOpto(const bool on) {
  digitalWrite(kOptoPin, on ? HIGH : LOW);
  Serial.println(on ? "GPIO13 ON (type 'off' to stop)" : "GPIO13 OFF");
}

void setup() {
  // Set LOW before enabling the outputs so nothing glitches HIGH.
  digitalWrite(kOptoPin, LOW);
  digitalWrite(kSwitchPinA, LOW);
  digitalWrite(kSwitchPinB, LOW);
  pinMode(kOptoPin, OUTPUT);
  pinMode(kSwitchPinA, OUTPUT);
  pinMode(kSwitchPinB, OUTPUT);

  Serial.begin(115200);
  Serial.setTimeout(50);
  Serial.println();
  Serial.println("Opto 1 test: GPIO13 only. GPIO14/GPIO26 held LOW.");
  Serial.println("Type 'on' or 'off'.");
}

void loop() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    command.toLowerCase();
    if (command == "on") {
      setOpto(true);
    } else if (command == "off") {
      setOpto(false);
    } else if (command.length() > 0) {
      Serial.println("Type 'on' or 'off'.");
    }
  }
}
