#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

#include <cinttypes>

#include "driver/gpio.h"
#include "driver/mcpwm.h"
#include "esp_err.h"

namespace {

// Pin map from the requested wiring.
constexpr gpio_num_t kRelayPin = GPIO_NUM_13;   // Optocoupler 1: relay only
constexpr gpio_num_t kSwitchPinA = GPIO_NUM_14; // TLP250 2 and 3: diagonal pair A
constexpr gpio_num_t kSwitchPinB = GPIO_NUM_26; // TLP250 4 and 5: diagonal pair B

// MCPWM clocking (ESP-IDF v4.4.7 legacy driver, Arduino-ESP32 2.0.17).
// The driver default is group 10 MHz / timer 1 MHz, which turns a 150 kHz
// request into peak = 1 MHz / 150 kHz = 6 ticks -> 166.7 kHz. Run both at
// 80 MHz instead: peak = 533 ticks -> 150.09 kHz with ~0.28 kHz tuning steps.
constexpr uint32_t kMcpwmBaseClockHz = 160000000UL; // SOC_MCPWM_BASE_CLK_HZ
constexpr uint32_t kMcpwmGroupResolutionHz = 80000000UL;
constexpr uint32_t kMcpwmTimerResolutionHz = 80000000UL;
static_assert(kMcpwmBaseClockHz % kMcpwmGroupResolutionHz == 0,
              "Group resolution must divide the 160 MHz MCPWM base clock");
static_assert(kMcpwmGroupResolutionHz % kMcpwmTimerResolutionHz == 0,
              "Timer resolution must divide the group resolution");

// Target waveform: 150 kHz, period 6.667 us, nominal half-cycle 3.333 us.
// Adjustable at runtime with 'freq <hz>' only while switching is stopped.
constexpr uint32_t kDefaultSwitchingFrequencyHz = 150000UL;
constexpr uint32_t kMinSwitchingFrequencyHz = 100000UL;
constexpr uint32_t kMaxSwitchingFrequencyHz = 200000UL;
constexpr float kNominalDutyPercent = 50.0f;
static_assert(kMcpwmTimerResolutionHz / kMinSwitchingFrequencyHz <= 65536UL,
              "MCPWM period register is 16 bits");

// Dead time. The legacy driver counts dead time in MCPWM *group* clock ticks
// (dt_clk_sel = 0), so at 80 MHz one tick is 12.5 ns. Reading the v4.4.7
// driver/LL code, the programmed delay appears to be (ticks + 1); the +1 tick
// is 12.5 ns extra margin. Confirm the real gap on the scope.
//
// 700 ns starting value: TLP250 tpLH/tpHL are 0.15 us typ, 0.5 us max, so
// opto-to-opto skew can approach 0.5 us; IRF840 td(off) + tf is ~69 ns.
// Reduce only after measuring gate-to-source waveforms on the real board.
constexpr uint32_t kDeadTimeNs = 700UL;
constexpr uint32_t kDeadTimeTicks = static_cast<uint32_t>(
    (static_cast<uint64_t>(kDeadTimeNs) * kMcpwmGroupResolutionHz +
     999999999ULL) /
    1000000000ULL);
static_assert(kDeadTimeTicks >= 1 && kDeadTimeTicks < 65535UL,
              "Dead-time register is 16 bits");
static_assert(kDeadTimeNs * 4UL < 1000000000UL / kMaxSwitchingFrequencyHz,
              "Dead time must stay well below half of the shortest period");

// After mcpwm_deadtime_enable() the new delays latch on the next timer zero.
// Wait a few periods before routing MCPWM to the pins.
constexpr uint32_t kMcpwmSettleUs = 50UL;

// Relays move in milliseconds, while the inverter switches in microseconds.
// These are conservative defaults for bench bring-up; use the relay datasheet.
constexpr uint32_t kPwmPrechargeMs = 20UL;
constexpr uint32_t kRelaySettleMs = 50UL;
constexpr uint32_t kRelayDropoutMs = 80UL;

// Armed 2026-10-11 at user request after the user reported rewiring to a half
// bridge: GPIO14 -> high side, GPIO26 -> low side, coil + series capacitor
// from switch node to GND, every gate on its own TLP250. NOT scope-verified.
// First powered test: current-limited 2-3 V bus.
constexpr bool kPowerStageTopologyVerified = true;
constexpr bool kAllowRelayEnergizedRun = true;

// IR obstacle sensor (FC-51 style, LM393 output) on GPIO27. Power the module
// from 3.3 V: at 5 V its pull-up drives 5 V into the ESP32 pin.
// "IR on" = sensor detecting (output LOW, module LED lit).
//   IR on  -> servo 45 deg, PWM + GPIO13 ON (wireless charging)
//   IR off -> servo 0 deg, GPIO13 OFF, then PWM OFF
// Acts on every IR change and once at startup, so it also runs at boot.
constexpr uint8_t kIrPin = 27;
constexpr bool kIrActiveLow = true;
constexpr uint32_t kIrDebounceMs = 100UL;
constexpr uint32_t kIrStartDelayMs = 2000UL; // after boot, before IR acts

// Servo signal on GPIO25, LEDC 50 Hz. Power the servo from a separate 5 V
// supply with GND shared with the ESP32 (servo current spikes can reset the
// ESP32 if it shares the USB 5 V).
constexpr uint8_t kServoPin = 25;
constexpr uint8_t kServoLedcChannel = 0;
constexpr uint32_t kServoFrequencyHz = 50UL;
constexpr uint8_t kServoResolutionBits = 16;
constexpr uint32_t kServoMinPulseUs = 500UL;  // 0 deg (typical SG90/MG90S)
constexpr uint32_t kServoMaxPulseUs = 2500UL; // 180 deg
constexpr uint32_t kServoIdleAngleDeg = 0UL;
constexpr uint32_t kServoTransferAngleDeg = 45UL;

// TLP250 is non-inverting: input LED ON -> output HIGH -> MOSFET ON. This is
// true only if the GPIO sources current into the LED anode. If the GPIO sinks
// the LED cathode instead, the command is inverted and the forced-LOW safe
// state would turn every MOSFET ON, so the firmware refuses to build that way.
constexpr bool kGateCommandActiveHigh = true;
static_assert(kGateCommandActiveHigh,
              "Safe state forces switch pins LOW; inverted gate drive needs a "
              "redesigned safe state before this can be changed");

constexpr mcpwm_unit_t kMcpwmUnit = MCPWM_UNIT_0;
constexpr mcpwm_timer_t kMcpwmTimer = MCPWM_TIMER_0;

constexpr size_t kMaxCommandLength = 48;

// 16x2 LCD with I2C backpack: SDA GPIO21, SCL GPIO22 (ESP32 defaults).
// Change to 0x3F if your backpack uses a PCF8574A.
constexpr uint8_t kLcdAddress = 0x27;
constexpr uint32_t kLcdUpdateMs = 500UL;

enum class ControllerState : uint8_t {
  Idle,
  StartingPwm,
  WaitingRelaySettle,
  Running,
  StoppingRelayDropout,
  FaultLocked
};

ControllerState g_state = ControllerState::Idle;
uint32_t g_stateEnteredMs = 0;
bool g_switchingActive = false;
bool g_irDetecting = false;
uint32_t g_servoAngleDeg = kServoIdleAngleDeg;
uint32_t g_switchingFrequencyHz = kDefaultSwitchingFrequencyHz;
const char *g_faultReason = "none";
LiquidCrystal_I2C g_lcd(kLcdAddress, 16, 2);

void relayOff() {
  gpio_set_level(kRelayPin, 0);
}

void relayOn() {
  gpio_set_level(kRelayPin, 1);
}

uint32_t actualFrequencyHz(const uint32_t requested_hz) {
  // Mirrors mcpwm_init(): peak = timer_clk / frequency (integer division).
  return kMcpwmTimerResolutionHz / (kMcpwmTimerResolutionHz / requested_hz);
}

void configureSafeGpio() {
  // Latch LOW in the output register before enabling the output drivers.
  gpio_set_level(kRelayPin, 0);
  gpio_set_level(kSwitchPinA, 0);
  gpio_set_level(kSwitchPinB, 0);

  // INPUT_OUTPUT so gpio_get_level() reads back the real pad level for the
  // relay status; with output-only mode the input path is off and reads 0.
  const gpio_config_t output_config = {
      .pin_bit_mask = (1ULL << kRelayPin) | (1ULL << kSwitchPinA) |
                      (1ULL << kSwitchPinB),
      .mode = GPIO_MODE_INPUT_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  gpio_config(&output_config);
}

// Detaches GPIO14/GPIO26 from MCPWM immediately: gpio_config() reconnects each
// pad to SIG_GPIO_OUT_IDX, whose output register is already LOW. This is the
// gate-kill path and must not depend on MCPWM state.
void forceSwitchPinsLowAsGpio() {
  gpio_set_level(kSwitchPinA, 0);
  gpio_set_level(kSwitchPinB, 0);

  const gpio_config_t switch_output_config = {
      .pin_bit_mask = (1ULL << kSwitchPinA) | (1ULL << kSwitchPinB),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  gpio_config(&switch_output_config);
}

void enterState(const ControllerState next) {
  g_state = next;
  g_stateEnteredMs = millis();
}

// Latches until reset. Relay OFF and gates OFF immediately, without waiting
// for relay dropout: on a fault, removing gate drive takes priority.
void latchFault(const char *reason) {
  relayOff();
  forceSwitchPinsLowAsGpio();
  if (g_switchingActive) {
    mcpwm_stop(kMcpwmUnit, kMcpwmTimer);
    g_switchingActive = false;
  }
  g_faultReason = reason;
  enterState(ControllerState::FaultLocked);
  Serial.printf("FAULT LATCHED: %s. Outputs safe; reset required.\n", reason);
}

bool checkEsp(const esp_err_t err, const char *operation) {
  if (err == ESP_OK) {
    return true;
  }

  Serial.printf("FAULT: %s failed: %s\n", operation, esp_err_to_name(err));
  latchFault(operation);
  return false;
}

bool startSwitchingOutputs() {
  relayOff();
  forceSwitchPinsLowAsGpio();

  // Configure MCPWM completely while the pins are still plain GPIO LOW.
  // mcpwm_init() starts the timer itself, and the dead-time module resets to
  // bypass, so A and B are briefly identical (both HIGH = both diagonal pairs
  // ON). Routing the pins only after dead time is active avoids that window.
  if (!checkEsp(mcpwm_group_set_resolution(kMcpwmUnit, kMcpwmGroupResolutionHz),
                "mcpwm_group_set_resolution")) {
    return false;
  }
  if (!checkEsp(mcpwm_timer_set_resolution(kMcpwmUnit, kMcpwmTimer,
                                           kMcpwmTimerResolutionHz),
                "mcpwm_timer_set_resolution")) {
    return false;
  }

  const mcpwm_config_t pwm_config = {
      .frequency = g_switchingFrequencyHz,
      .cmpr_a = kNominalDutyPercent,
      .cmpr_b = kNominalDutyPercent,
      .duty_mode = MCPWM_DUTY_MODE_0,
      .counter_mode = MCPWM_UP_COUNTER,
  };

  if (!checkEsp(mcpwm_init(kMcpwmUnit, kMcpwmTimer, &pwm_config),
                "mcpwm_init")) {
    return false;
  }
  g_switchingActive = true; // timer is running internally from here on

  if (!checkEsp(mcpwm_deadtime_enable(kMcpwmUnit, kMcpwmTimer,
                                      MCPWM_ACTIVE_HIGH_COMPLIMENT_MODE,
                                      kDeadTimeTicks, kDeadTimeTicks),
                "mcpwm_deadtime_enable")) {
    return false;
  }

  delayMicroseconds(kMcpwmSettleUs);

  if (!checkEsp(mcpwm_gpio_init(kMcpwmUnit, MCPWM0A, kSwitchPinA),
                "mcpwm_gpio_init A")) {
    return false;
  }
  if (!checkEsp(mcpwm_gpio_init(kMcpwmUnit, MCPWM0B, kSwitchPinB),
                "mcpwm_gpio_init B")) {
    return false;
  }

  return true;
}

void stopSwitchingOutputs() {
  // Kill the pins first; MCPWM cleanup below can no longer reach them.
  forceSwitchPinsLowAsGpio();

  if (g_switchingActive) {
    g_switchingActive = false;
    checkEsp(mcpwm_stop(kMcpwmUnit, kMcpwmTimer), "mcpwm_stop");
    checkEsp(mcpwm_deadtime_disable(kMcpwmUnit, kMcpwmTimer),
             "mcpwm_deadtime_disable");
  }
}

bool isFaulted() {
  return g_state == ControllerState::FaultLocked;
}

void stopController() {
  if (isFaulted()) {
    relayOff();
    stopSwitchingOutputs();
    Serial.printf("Fault latched (%s); outputs forced safe. Reset required.\n",
                  g_faultReason);
    return;
  }

  relayOff();
  if (g_switchingActive) {
    enterState(ControllerState::StoppingRelayDropout);
  } else {
    stopSwitchingOutputs();
    if (!isFaulted()) {
      enterState(ControllerState::Idle);
    }
  }
}

bool canRunPoweredStage() {
  return kPowerStageTopologyVerified && kAllowRelayEnergizedRun;
}

bool requireIdle(const char *command) {
  if (g_state == ControllerState::Idle) {
    return true;
  }
  Serial.printf("Refusing %s: controller is not Idle. Send 'stop' first.\n",
                command);
  return false;
}

// Called only by the IR sensor; there is no manual turn-on command.
void requestPoweredRun() {
  if (!canRunPoweredStage()) {
    Serial.println("Refusing RUN: topology/polarity are not verified in firmware.");
    return;
  }
  if (!requireIdle("run")) {
    return;
  }

  relayOff();
  if (startSwitchingOutputs()) {
    enterState(ControllerState::StartingPwm);
    Serial.println("PWM started with relay OFF; waiting before relay energize.");
  }
}

bool parseUnsigned(const String &text, uint32_t &value) {
  if (text.length() == 0 || text.length() > 7) {
    return false;
  }
  uint32_t parsed = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10UL + static_cast<uint32_t>(c - '0');
  }
  value = parsed;
  return true;
}

void requestFrequencyChange(const String &argument) {
  if (!requireIdle("freq")) {
    return;
  }

  uint32_t requested_hz = 0;
  if (!parseUnsigned(argument, requested_hz) ||
      requested_hz < kMinSwitchingFrequencyHz ||
      requested_hz > kMaxSwitchingFrequencyHz) {
    Serial.printf("Usage: freq <hz>, %" PRIu32 "..%" PRIu32 "\n", kMinSwitchingFrequencyHz,
                  kMaxSwitchingFrequencyHz);
    return;
  }

  g_switchingFrequencyHz = requested_hz;
  Serial.printf("Frequency set: requested %" PRIu32 " Hz, actual %" PRIu32 " Hz.\n",
                requested_hz, actualFrequencyHz(requested_hz));
}

const char *stateName(const ControllerState state) {
  switch (state) {
  case ControllerState::Idle:
    return "Idle";
  case ControllerState::StartingPwm:
    return "StartingPwm";
  case ControllerState::WaitingRelaySettle:
    return "WaitingRelaySettle";
  case ControllerState::Running:
    return "Running";
  case ControllerState::StoppingRelayDropout:
    return "StoppingRelayDropout";
  case ControllerState::FaultLocked:
    return "FaultLocked";
  }
  return "Unknown";
}

// LCD labels, at most 10 characters so line 1 fits 16 columns.
const char *stateShortName(const ControllerState state) {
  switch (state) {
  case ControllerState::Idle:
    return "IDLE";
  case ControllerState::StartingPwm:
    return "STARTING";
  case ControllerState::WaitingRelaySettle:
    return "RLY SETTLE";
  case ControllerState::Running:
    return "RUNNING";
  case ControllerState::StoppingRelayDropout:
    return "STOPPING";
  case ControllerState::FaultLocked:
    return "FAULT";
  }
  return "UNKNOWN";
}

// Line 1: "RUNNING    R:ON "   Line 2: "150.09kHz SW:ON "
void updateLcd() {
  static uint32_t last_update_ms = 0;
  if (millis() - last_update_ms < kLcdUpdateMs) {
    return;
  }
  last_update_ms = millis();

  char line1[24];
  char line2[24];
  if (isFaulted()) {
    snprintf(line1, sizeof(line1), "%-16s", "FAULT - RESET");
    snprintf(line2, sizeof(line2), "%-16.16s", g_faultReason);
  } else {
    const uint32_t hz = actualFrequencyHz(g_switchingFrequencyHz);
    snprintf(line1, sizeof(line1), "%-10s R:%-3s", stateShortName(g_state),
             gpio_get_level(kRelayPin) ? "ON" : "OFF");
    snprintf(line2, sizeof(line2), "%3" PRIu32 ".%02" PRIu32 "kHz SW:%-3s",
             hz / 1000U, (hz % 1000U) / 10U, g_switchingActive ? "ON" : "OFF");
  }

  g_lcd.setCursor(0, 0);
  g_lcd.print(line1);
  g_lcd.setCursor(0, 1);
  g_lcd.print(line2);
}

void printStatus() {
  Serial.printf("state=%s fault=%s relay=%s switching=%s\n",
                stateName(g_state), g_faultReason,
                gpio_get_level(kRelayPin) ? "ON" : "OFF",
                g_switchingActive ? "ON" : "OFF");
  Serial.printf("freq requested=%" PRIu32 " Hz actual=%" PRIu32 " Hz deadtime=%" PRIu32 " ns (%" PRIu32 " ticks @ %" PRIu32 " MHz)\n",
                g_switchingFrequencyHz, actualFrequencyHz(g_switchingFrequencyHz),
                kDeadTimeNs, kDeadTimeTicks, static_cast<uint32_t>(kMcpwmGroupResolutionHz / 1000000UL));
  Serial.printf("ir=%s servo=%" PRIu32 " deg\n", g_irDetecting ? "on" : "off",
                g_servoAngleDeg);
  Serial.printf("topologyVerified=%s relayRunAllowed=%s gateActive=%s\n",
                kPowerStageTopologyVerified ? "true" : "false",
                kAllowRelayEnergizedRun ? "true" : "false",
                kGateCommandActiveHigh ? "HIGH" : "LOW");
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  (Turn-on is IR only: IR on -> servo 45 deg + charge, IR off -> stop)");
  Serial.println("  stop       Relay OFF, then stop PWM after dropout delay");
  Serial.println("  freq <hz>  Set switching frequency, 100000..200000 (Idle only)");
  Serial.println("  status     Print controller state");
  Serial.println("  help       Print this help");
  Serial.println();
}

void handleSerialCommand(const String &raw_command) {
  String command = raw_command;
  command.trim();
  command.toLowerCase();
  if (command.length() == 0) {
    return;
  }

  if (command == "stop") {
    stopController();
  } else if (command.startsWith("freq ")) {
    String argument = command.substring(5);
    argument.trim();
    requestFrequencyChange(argument);
  } else if (command == "status") {
    printStatus();
  } else if (command == "help") {
    printHelp();
  } else {
    Serial.println("Unknown command. Type 'help'.");
  }
}

void serviceSerial() {
  static String command;

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      handleSerialCommand(command);
      command = "";
    } else if (command.length() < kMaxCommandLength) {
      command += c;
    } else {
      command = "";
      Serial.println("Command too long; cleared.");
    }
  }
}

void serviceStateMachine() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - g_stateEnteredMs;

  switch (g_state) {
  case ControllerState::StartingPwm:
    if (elapsed >= kPwmPrechargeMs) {
      relayOn();
      enterState(ControllerState::WaitingRelaySettle);
      Serial.println("Relay energized; waiting for contact settling.");
    }
    break;

  case ControllerState::WaitingRelaySettle:
    if (elapsed >= kRelaySettleMs) {
      enterState(ControllerState::Running);
      Serial.println("RUNNING.");
    }
    break;

  case ControllerState::StoppingRelayDropout:
    if (elapsed >= kRelayDropoutMs) {
      stopSwitchingOutputs();
      if (!isFaulted()) {
        enterState(ControllerState::Idle);
        Serial.println("Stopped: relay OFF, PWM OFF, switch pins LOW.");
      }
    }
    break;

  case ControllerState::Idle:
  case ControllerState::Running:
  case ControllerState::FaultLocked:
    break;
  }
}

void setServoAngle(const uint32_t degrees) {
  const uint32_t pulse_us =
      kServoMinPulseUs + (kServoMaxPulseUs - kServoMinPulseUs) * degrees / 180UL;
  const uint32_t period_us = 1000000UL / kServoFrequencyHz;
  const uint32_t max_duty = (1UL << kServoResolutionBits) - 1UL;
  ledcWrite(kServoLedcChannel, pulse_us * max_duty / period_us);
  g_servoAngleDeg = degrees;
}

bool readIrDetecting() {
  const int level = digitalRead(kIrPin);
  return kIrActiveLow ? level == LOW : level == HIGH;
}

// The only way to turn on. A serial 'stop' holds until the IR state changes.
void serviceIrControl() {
  static bool have_state = false;
  static bool last_raw = false;
  static uint32_t raw_since_ms = 0;
  static bool run_pending = false;

  const uint32_t now = millis();
  const bool raw = readIrDetecting();
  if (raw != last_raw) {
    last_raw = raw;
    raw_since_ms = now;
  }

  if (now >= kIrStartDelayMs && now - raw_since_ms >= kIrDebounceMs &&
      (!have_state || raw != g_irDetecting)) {
    have_state = true;
    g_irDetecting = raw;
    if (g_irDetecting) {
      Serial.println("IR on: servo 45 deg, transfer ON.");
      setServoAngle(kServoTransferAngleDeg);
      run_pending = true;
    } else {
      Serial.println("IR off: servo 0 deg, transfer OFF.");
      setServoAngle(kServoIdleAngleDeg);
      run_pending = false;
      stopController();
    }
  }

  // run only starts from Idle, e.g. after a quick IR off -> on while the
  // stop sequence is still waiting for relay dropout.
  if (run_pending && g_state == ControllerState::Idle) {
    run_pending = false;
    requestPoweredRun();
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  configureSafeGpio();
  stopSwitchingOutputs();
  enterState(ControllerState::Idle);

  Serial.println();
  Serial.println("ESP32 150 kHz WPT transmitter controller");
  Serial.println("Safe state: relay OFF, switching outputs inactive.");

  pinMode(kIrPin, INPUT_PULLUP);
  ledcSetup(kServoLedcChannel, kServoFrequencyHz, kServoResolutionBits);
  ledcAttachPin(kServoPin, kServoLedcChannel);
  setServoAngle(kServoIdleAngleDeg);

  // After the safe state: init() starts Wire on GPIO21/22 and takes ~1 s.
  g_lcd.init();
  g_lcd.backlight();

  printHelp();
  printStatus();
  Serial.println("IR control starts in 2 s.");
}

void loop() {
  serviceSerial();
  serviceIrControl();
  serviceStateMachine();
  updateLcd();
  delay(1);
}
