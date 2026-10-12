# Project Memory - ESP32 150 kHz WPT Controller

Last updated: 2026-10-11.

This file is the durable memory for future AI agents. Keep it current whenever
firmware behavior, hardware assumptions, or verification status changes.

Legend: `[CONFIRMED]` seen in code, build, or source; `[USER]` stated by the
user, not measured; `[ASSUMPTION]` chosen default; `[TODO]` open item;
`[BLOCKER]` must be resolved before trusting powered operation.

## Current State (end of 2026-10-11)

- Power stage: IRF840 **half bridge**, TLP250 gate drivers, 12 V supply
  through a separate "power supply module" whose IRF840 switch (GPIO13) runs
  hot. See "Supply Module" below.
- Firmware: run is **armed**. The IR sensor is the only way to turn on
  (IR on -> servo 45 deg + charge, IR off -> servo 0 deg + stop). The serial
  `run` and `pwm` commands were removed 2026-10-12 at user request.
- Board currently flashed (2026-10-12) with the IR-only firmware (no serial
  `run`/`pwm`). Replaces the old `b9dd578` boot auto-run firmware. An object
  in front of the IR sensor at boot still starts charging 2 s after every
  reset (including opening a serial monitor). Not tested on hardware yet.
- Git: public repo https://github.com/irfanmuhammedharis/esp32-wireless-v2.
  Commit `b9dd578` is the state before the IR/servo feature. The IR-only
  firmware and these docs were committed and pushed 2026-10-12.
- Nothing has been measured with a scope: gate waveforms, dead time, bus
  current, and capacitor value are all unverified.

## Build And Toolchain

```bash
/home/irfan/.platformio/penv/bin/platformio run -e esp32dev            # main firmware
/home/irfan/.platformio/penv/bin/platformio run -e esp32dev -t upload  # flash main
/home/irfan/.platformio/penv/bin/platformio run -e opto13_test -t upload  # GPIO13-only test
```

- `[CONFIRMED]` Platform pinned `espressif32 @ 7.0.0` = Arduino-ESP32 2.0.17
  (`framework-arduinoespressif32 @ 3.20017.241212`) = ESP-IDF v4.4.7. Pinned
  because the firmware uses the legacy `driver/mcpwm.h` API.
- `[CONFIRMED]` `default_envs = esp32dev`, so a plain build or upload never
  flashes the test program by mistake.
- `[CONFIRMED]` `upload_speed = 115200`. 460800 fails on this board's
  USB-serial link ("No serial data received" after the baud change).
- `[CONFIRMED]` Project sources build with `-Wall -Wextra -Werror`. On this
  toolchain `uint32_t` is `unsigned int`, so use `PRIu32` in printf, not `%lu`.
- Library: `marcoschwartz/LiquidCrystal_I2C @ 1.1.4` (user's choice). Its
  library.json lists only AVR/ESP8266, but it builds under the default
  `lib_compat_mode = soft`.
- Last build (IR/servo version): RAM 22148 B (6.8%), Flash 305313 B (23.3%).
- Opening any serial monitor resets the ESP32 (auto-reset circuit). The Arduino
  IDE serial monitor holding `/dev/ttyUSB0` makes uploads fail with "port is
  busy".

## Pin Map

| GPIO | Function | Firmware treatment |
|---|---|---|
| 13 | Opto 1 -> supply-module IRF840 switch ("relay") | Default OFF. ON in the run sequence 20 ms after PWM starts. |
| 14 | TLP250 2+3 -> half-bridge **high side** | MCPWM0A, complementary with dead time. |
| 26 | TLP250 4+5 -> half-bridge **low side** | MCPWM0B = inverse of A with dead time. |
| 21 / 22 | LCD I2C SDA / SCL | LiquidCrystal_I2C at 0x27. |
| 27 | IR obstacle sensor OUT | `INPUT_PULLUP`, LOW = "IR on" (detecting). |
| 25 | Servo signal | LEDC ch 0, 50 Hz, 500-2500 us = 0-180 deg. |

GPIO13/14/26 are configured `GPIO_MODE_INPUT_OUTPUT` so `gpio_get_level()`
reads back the real pad level. Output-only mode reads 0 and showed
`relay=OFF` while running (bug found and fixed on hardware).

## Firmware Behavior (`src/main.cpp`)

- Safe boot: GPIO13/14/26 LOW, servo 0 deg, state `Idle`.
- MCPWM group and timer clocks at 80 MHz: 150 kHz request -> 533 ticks ->
  **150.09 kHz** actual, ~0.28 kHz steps.
- Dead time **700 ns** = 56 ticks of the 12.5 ns group clock (from TLP250
  0.5 us max delay + IRF840 ~69 ns turn-off). Not measured.
- Start order: MCPWM is configured and running with dead time active while the
  pins are plain GPIO LOW; the pins are routed to MCPWM last.
- Stop/fault order: the pins are detached and forced LOW first, then the MCPWM
  timer stops.
- Run sequence (IR on): PWM on with GPIO13 OFF -> 20 ms -> GPIO13 ON -> 50 ms ->
  `Running`. `stop`: GPIO13 OFF -> 80 ms -> PWM OFF -> `Idle`.
- `FaultLocked` is sticky: any MCPWM API error turns GPIO13 and the gates OFF
  immediately. Only a reset clears it.
- Serial 115200 commands:

  | Command | Behavior |
  |---|---|
  | `stop` | Turns everything off in order. Always allowed. |
  | `freq <hz>` | 100000..200000, Idle only, applies on next start. |
  | `status` | State, fault, relay, switching, frequency, dead time, IR, servo. |

- IR + servo control (user request; replaced the earlier boot auto-run):
  - IR on (GPIO27 LOW): servo to 45 deg, then run. Wireless charging ON.
  - IR off (HIGH): servo to 0 deg, then `stop`.
  - 100 ms debounce. It acts 2 s after boot and on every change, so an object
    already present at boot starts charging by itself. A serial `stop` holds
    until the IR state changes. Flip polarity with `kIrActiveLow`.
- LCD (simple, user asked to keep it simple): every 500 ms, line 1
  `RUNNING    R:ON`, line 2 `150.09kHz SW:ON`, or `FAULT - RESET` + the
  reason. `lcd.init()` blocks ~1.1 s at boot; each refresh blocks the loop
  ~40 ms. No I2C error detection. `[USER]` The display works (showed the OFF
  state).
- `src/opto13_test.cpp` (env `opto13_test`): drives only GPIO13 via serial
  `on`/`off`, with no timeout (user request). GPIO14/26 are held LOW.

## Arm Flags

```cpp
constexpr bool kPowerStageTopologyVerified = true;   // armed
constexpr bool kAllowRelayEnergizedRun = true;       // armed
constexpr bool kGateCommandActiveHigh = true;        // static_assert blocks false
```

Arm history (all 2026-10-11, all at the user's explicit request, no scope):
1. Armed for an assumed full bridge; `run` at 12 V. Parts got warm.
2. The user then said it is a **half bridge**, so I re-locked it. The user's
   hand drawing showed two gates wired to the coil and no series capacitor.
3. The user reported rewiring as the reference half bridge (below), so I armed
   it again. On `run`, the supply-module IRF840 got hot and the run was stopped.

`kGateCommandActiveHigh` must stay true. TLP250 is non-inverting, and the
safe state forces the pins LOW, so inverted drive would turn every MOSFET ON.

## Hardware Facts

- `[USER]` Half bridge: Q1 (high) drain to +12 V, Q1 source = switch node =
  Q2 drain, Q2 source to GND. Coil + series capacitor from the switch node to
  GND. Each gate on its own TLP250. Value and rating of the series capacitor
  are unknown.
- `[USER]` MOSFETs: IRF840 (500 V, 8 A, RDS(on) 0.85 ohm max at 25 C, ~1.5 ohm
  hot, Qg 63 nC). Poor fit for 12 V; IRFZ44N (17.5 mohm) is the suggested
  replacement.
- `[USER]` Coils: TX and RX identical, 1.0 mm wire, 20 turns, 10 cm diameter.
  `[ASSUMPTION]` L ~79 uH, R ~0.2 ohm at 150 kHz. Measure L with an LCR meter.
- `[BLOCKER]` High-side TLP250 supply (floating 15 V or bootstrap) never
  confirmed.

## Supply Module (GPIO13 "relay")

`[USER]` It is not a relay. It is a perfboard "power supply module": input
terminal -> cement power resistor -> large electrolytic -> 1 mH/10 A E25 choke
-> **IRF840 (source to GND, gate driven at 12 V via opto 1, no heatsink)** ->
diode -> small electrolytic -> second 1 mH/10 A choke -> output terminal.

- That layout is a **boost converter**. If the drain sits at the choke/diode
  junction, holding GPIO13 ON shorts 12 V through the resistor, choke, and
  IRF840; the diode then blocks and the output gets almost nothing.
- If it is instead a plain low-side switch, the heat is I^2 x 0.85-1.5 ohm
  (1.5 A -> 2-3.4 W -> +125-210 C on a bare TO-220 at 62 C/W).
- `[TODO]` Pending test (`opto13_test`): half bridge disconnected, 12 V with a
  1 A limit, `on` -> read supply current and module output voltage. High
  current with ~0 V out means boost short; ~0 A with 12 V out means a switch.
- Resistor value and capacitor values are unread.

## Power Stage Analysis (half bridge, calculated, not measured)

- Half-bridge fundamental = 0.45 x 12 V = 5.4 V rms. One IRF840 conducts at a
  time, so loop R ~ 0.2 + 0.85 = ~1.05 ohm.
- Series capacitor for 150 kHz with L 79 uH: C = 14.2 nF (10 nF + 4.7 nF =
  14.7 nF -> 147.7 kHz). Polypropylene film, >= 630 V DC. In a half bridge it
  also blocks the 6 V DC midpoint.
- Without the capacitor: ~0.07 A, almost no transfer.
- **Hazard:** at resonance with no receiver, ~5.1 A rms and ~380 V rms across
  the capacitor. The current rises as the receiver moves away. Start at 2-3 V,
  current-limited.
- At a 5 cm gap (k ~0.125): ~1.5 W at 90% (5 ohm load), ~2.7 W at 87%
  (10 ohm load).
- Run slightly **above** resonance: IRF840 body-diode recovery is slow, and
  running below resonance causes destructive hard switching.

## MCPWM Driver Facts (ESP-IDF v4.4.7 source)

- `[CONFIRMED]` Legacy driver defaults to group 10 MHz / timer 1 MHz, and
  `peak = timer_clk / f` uses integer division (the original firmware made
  166.7 kHz).
- `[CONFIRMED]` `mcpwm_init()` starts the timer. The dead-time block resets to
  bypass, so A and B are identical until `mcpwm_deadtime_enable()`. The
  original firmware turned all switches ON for microseconds at every start.
  Fixed by routing the pins last.
- `[CONFIRMED]` Dead-time ticks use the group clock (`dt_clk_sel = 0`).
- `[ASSUMPTION]` The real delay is probably `ticks + 1` (712.5 ns).
- `[CONFIRMED]` Changing frequency on a running timer updates the period
  immediately and can stretch a cycle; that is why `freq` is Idle-only.

## Open Risks

- `[BLOCKER]` Supply-module IRF840 overheats whenever GPIO13 is ON.
- `[BLOCKER]` No scope data: dead time, gate-to-source voltages, and that the
  high and low sides are never ON together.
- `[BLOCKER]` High-side TLP250 floating supply / bootstrap unconfirmed.
- `[BLOCKER]` No overcurrent protection, fuse, or MCPWM fault input.
- `[TODO]` One GPIO drives two TLP250 LEDs (~20 mA total, at the drive limit);
  use a buffer.
- `[TODO]` GPIO13/14 are JTAG pins and are uncontrolled during boot. Fit
  external pull-downs on all opto inputs.
- `[TODO]` IR module on 3.3 V; servo on its own 5 V with shared GND (SG90 stall
  current ~650 mA can reset the ESP32 on USB).
- `[TODO]` LCD backpack pull-ups to 5 V exceed ESP32 pin ratings; use a level
  shifter or 3.3 V pull-ups.
- `[TODO]` No watchdog. If the firmware hangs while running, MCPWM keeps
  switching.

## Next Recommended Tasks

1. Run the `opto13_test` supply-module test and decide: replace the module
   (P-ch IRF4905 high-side switch, or feed the bridge straight from the bench
   supply) or fix it (IRFZ44N + heatsink).
2. Scope with IR on (supply disconnected; GPIO13 also turns ON): 6.66 us period, ~0.7 us both-LOW gaps,
   gate-to-source on both MOSFETs.
3. Measure coil L, fit the series capacitor, and commission at 2-3 V
   current-limited with the receiver present; tune `freq` just above resonance.
4. Replace the IRF840s with low-RDS(on) MOSFETs for the 12 V bus.

## Verification Checklist For Future Changes

- Firmware: build `esp32dev` (and `opto13_test` if touched) with zero
  warnings. Confirm safe boot (GPIO13 OFF, PWM OFF) in the serial log.
- Hardware-enabling changes: require schematic or scope evidence, or an
  explicit user request recorded here.
- Keep this file, `wpt-controller-design.md`, and `AGENTS.md` consistent with
  `src/main.cpp`.
