# Project Memory - ESP32 150 kHz WPT Controller

Last updated: 2026-10-11.

This file is the durable memory for future AI agents. Keep it current whenever
firmware behavior, hardware assumptions, or verification status changes.

## Current State

- `[CONFIRMED]` Project is a PlatformIO Arduino project.
- `[CONFIRMED]` PlatformIO environment is `[env:esp32dev]`.
- `[CONFIRMED]` Firmware lives in `src/main.cpp`.
- `[CONFIRMED]` Documentation lives in `docs/`.
- `[CONFIRMED]` Current firmware builds successfully for `esp32dev` using:

```bash
/home/irfan/.platformio/penv/bin/platformio run
```

Last known successful build:

- PlatformIO Core: 6.2.0.
- Platform: Espressif 32 7.0.0.
- Board: Espressif ESP32 Dev Module.
- Arduino-ESP32 package: `framework-arduinoespressif32 @ 3.20017.241212+sha.dcc1105b`.
- Platform pinned in `platformio.ini` as `espressif32 @ 7.0.0` (bundles
  ESP-IDF v4.4.7). Project sources build with `-Wall -Wextra -Werror`.
- Library: `marcoschwartz/LiquidCrystal_I2C @ 1.1.4` (LCD, user's choice).
  Its library.json lists only atmelavr/espressif8266, but it builds and is
  accepted under PlatformIO's default `lib_compat_mode = soft`.
- RAM usage: 22012 bytes, 6.7%.
- Flash usage: 298037 bytes, 22.7%.

## Firmware Summary

`src/main.cpp` implements:

- Safe boot: relay OFF, GPIO14 LOW, GPIO26 LOW.
- GPIO13/14/26 are configured `GPIO_MODE_INPUT_OUTPUT` at boot, so
  `gpio_get_level()` reads back the real relay pad level. With output-only
  mode the input path is off and always reads 0; that was a real bug that
  showed `relay=OFF` while running, fixed and verified on 2026-10-11.
- `[CONFIRMED on hardware 2026-10-11]` `run` -> `state=Running relay=ON
  switching=ON` at 150093 Hz; `stop` -> relay OFF, PWM OFF, Idle. Serial
  only: gate waveforms and bus current were not measured.
- MCPWM group and timer clocks set to 80 MHz. 150 kHz request -> peak 533
  ticks -> 150.09 kHz actual, ~0.28 kHz tuning steps near 150 kHz.
- Dead time: 700 ns = 56 ticks of the 12.5 ns group clock (see dead-time
  notes below).
- Start order: MCPWM is fully configured (timer running, dead time enabled)
  while GPIO14/GPIO26 are held LOW as plain GPIO; only then are the pins routed
  to MCPWM.
- Stop/fault order: GPIO14/GPIO26 are detached from MCPWM and forced LOW first,
  then the MCPWM timer is stopped.
- Serial command interface at 115200 baud.
- `pwm` command for relay-off oscilloscope testing (Idle only).
- `run` command locked out by default (Idle only when armed).
- `kAutoRunAtBoot = true` (user request 2026-10-11): 2 s after every boot or
  reset, the firmware runs the powered sequence by itself (PWM, then GPIO13
  20 ms later). Opening a serial monitor resets the board, so it also
  auto-runs. `stop` cancels a pending auto-run.
- `freq <hz>` sets 100000..200000 Hz, Idle only; applied on next start.
- `stop` command that turns relay OFF first, waits for dropout, then stops
  switching and forces GPIO14/GPIO26 LOW.
- `FaultLocked` is sticky: any MCPWM API failure turns the relay and gates OFF
  immediately and latches until reset. `stop` cannot clear it.

## 16x2 I2C LCD (in `src/main.cpp`)

- Simple LiquidCrystal_I2C code (user asked for simple, no extra module).
  Address `kLcdAddress = 0x27` (change to 0x3F for PCF8574A). SDA GPIO21,
  SCL GPIO22 (Wire defaults, started by `lcd.init()`).
- `updateLcd()` rewrites both lines every 500 ms. Line 1 shows state + relay
  (`RUNNING    R:ON`), line 2 shows actual frequency + switching
  (`150.09kHz SW:ON`). On a fault it shows `FAULT - RESET` + the reason.
- Cost: `lcd.init()` blocks ~1.1 s in `setup()` (after the safe state is
  set). Each refresh blocks the loop ~40 ms (32 chars x ~1.3 ms), which delays
  serial command handling by up to that much. No I2C error detection.
- `[TODO]` Backpack level: most backpacks run at 5 V with pull-ups to 5 V on
  SDA/SCL, and ESP32 pins are not 5 V tolerant. Use a BSS138/PCA9306 level
  shifter, or remove the backpack pull-ups and fit ~3.3 k to 3.3 V.
- `[TODO]` On the first hardware test (2026-10-11), nothing answered at
  0x27/0x3F. Check wiring and power.

## MCPWM Driver Facts (verified against ESP-IDF v4.4.7 source)

- `[CONFIRMED]` Default legacy-driver clocks are group 10 MHz, timer 1 MHz.
  `mcpwm_init()` computes `peak = timer_clk / frequency` with integer
  division, so the old firmware produced 1 MHz / 6 = 166.7 kHz, not 150 kHz.
- `[CONFIRMED]` `mcpwm_init()` starts the timer itself.
- `[CONFIRMED]` The dead-time module resets to bypass (`dt_a_outbypass` and
  `dt_b_outbypass` default 1). With equal A/B duty, A and B are identical until
  `mcpwm_deadtime_enable()` runs. The old firmware had the pins routed during
  that window, which turned both diagonal pairs ON together (shoot-through)
  for microseconds on every start. Fixed by routing pins last.
- `[CONFIRMED]` Dead-time ticks use the MCPWM group clock (`dt_clk_sel = 0`),
  not the timer clock. Changing group resolution changes dead-time units.
- `[ASSUMPTION]` The driver writes `red + 1` and the LL layer subtracts 1, so
  the real delay is probably `(ticks + 1)` group clocks (712.5 ns for 56
  ticks). Confirm on the scope.
- `[CONFIRMED]` Changing frequency on a running timer with this driver updates
  the period immediately. If the counter is above the new period, it can run on
  to 65535 with one output stuck ON. That is why `freq` is Idle-only.

## Pin And Signal Memory

| ESP32 GPIO | Function | Current firmware treatment |
|---|---|---|
| GPIO13 | Relay through Optocoupler 1 | Relay enable only, default OFF. |
| GPIO14 | TLP250 2 and 3 | MCPWM0A command A, diagonal pair A. |
| GPIO26 | TLP250 4 and 5 | MCPWM0B command B, diagonal pair B. |
| GPIO21 | LCD I2C SDA | 16x2 LCD backpack at 0x27, status only. |
| GPIO22 | LCD I2C SCL | 16x2 LCD backpack at 0x27, status only. |

Critical distinction: GPIO13 is not a 150 kHz output.

## Important Code Gates

In `src/main.cpp`, these constants intentionally block powered relay operation:

```cpp
constexpr bool kPowerStageTopologyVerified = false;
constexpr bool kAllowRelayEnergizedRun = false;
```

`[RE-LOCKED 2026-10-11]` Both were briefly `true` (user request), and `run`
was executed with 12 V applied. The user reported that something got warm.
The topology was then corrected to a half bridge, so both are `false` again
and the board is flashed with `run` refused. Do not re-arm until the coil
wiring is known, the high-side TLP250 floating supply is confirmed, and the
MOSFETs are checked for damage.

`[ARMED 2026-10-11, second time]` The user reports rewiring as the reference
half bridge: Q1 drain to +12 V, Q1 source = switch node = Q2 drain, Q2 source
to GND, coil + series capacitor from the switch node to GND, and each gate on
its own TLP250 (GPIO14 high side, GPIO26 low side). Flags are `true` again.
Not verified: scope waveforms, high-side TLP250 bootstrap/floating supply,
MOSFET damage from the earlier warm run, capacitor value/rating.

`[CONFIRMED by user 2026-10-11]` The "relay" on GPIO13/Opto 1 is a MOSFET
switch, NOT an electromechanical relay. During the second armed run it got
hot within seconds and the run was stopped. Likely causes:
(a) An N-channel MOSFET on the +12 V side without a gate voltage above 12 V
    acts as a source follower: it drops ~4-5 V at load, so 1 A means ~5 W.
(b) IRF840 RDS(on) 0.85 ohm (~1.5 ohm hot): 2 A means 3.4 W, and a TO-220
    without a heatsink (62 C/W) rises ~200 C.
(c) High bridge current at resonance.
Needed: MOSFET part, high or low side, gate driver/voltage, supply current
during the run. The relay timing constants (20/50/80 ms) are harmless with a
MOSFET.

Also verify this before changing it:

```cpp
constexpr bool kGateCommandActiveHigh = true;
```

TLP250 is non-inverting (LED ON -> output HIGH), so `true` is correct only if
each GPIO sources current into the TLP250 LED anode. A `static_assert` now
blocks `false`, because the forced-LOW safe state would then turn every MOSFET
ON. Inverted drive needs a redesigned safe state, not a flag flip.

## Hardware Facts From User (2026-10-11)

- `[CONFIRMED by user]` Gate drive: TLP250 optocoupler gate drivers.
- `[CONFIRMED by user]` MOSFETs: IRF840 (500 V, 8 A, RDS(on) 0.85 ohm max,
  Qg 63 nC max).
- `[CORRECTED by user 2026-10-11]` Topology is a HALF bridge, NOT a full
  bridge. GPIO14 (TLP250 2+3) drives the HIGH side, GPIO26 (TLP250 4+5)
  drives the LOW side. The complementary + dead-time MCPWM waveform suits
  this mapping.
- `[UNKNOWN]` Coil connection: the user answered "no midpoint" and "other".
  A schematic is needed. If one coil end goes from the switch node straight to
  GND or +12 V with no series capacitor, the coil carries ~5-6 A DC
  (~6 V across ~1 ohm).
- `[CONFIRMED by user]` DC bus: 12 V.
- `[CONFIRMED by user]` Transmitter coil: 1.0 mm wire, 20 turns, 10 cm
  diameter. Receiver coil is identical.
- `[CONFIRMED by user]` No resonant capacitor is fitted yet.
- `[ASSUMPTION]` Coil L ~79 uH (multi-turn loop estimate, ~84 uH by Wheeler).
  R ~0.2 ohm at 150 kHz. Measure L with an LCR meter.

## Power Stage Analysis (calculated, not measured)

- No capacitor: the bare coil is 74 ohm reactive at 150 kHz, so primary current
  is ~0.15 A rms and receiver open-circuit voltage is ~1.35 V rms at a 5 cm
  gap. Almost no power transfers. Series compensation is needed on both coils.
- Series-series (SS) compensation: C = 1 / ((2*pi*150 kHz)^2 * 79 uH) =
  14.2 nF on each side. Example: 10 nF + 4.7 nF = 14.7 nF -> 147.7 kHz.
  Use polypropylene film (MKP/CBB), rated >= 630 V DC.
- SS hazard: primary current rises as the receiver moves away. With no
  receiver at resonance, current is limited only by ~1.9 ohm (coil + 2x IRF840):
  12 V bus -> ~5.7 A rms and ~420 V rms across the transmitter capacitor.
- At a 5 cm gap (k ~0.125) with a 10 ohm AC load: ~9 W at ~80% efficiency
  with IRF840, versus ~12.7 W at ~95% with ~20 mohm MOSFETs. At a 10 cm gap,
  IRF840 efficiency drops to ~15-57%.
- IRF840 body diode recovery is slow (hundreds of ns). Operate slightly above
  resonance so the bridge sees an inductive load and soft switching (ZVS).
  Never run below resonance.
- IRF840 RDS(on) 0.85 ohm is a poor fit for a 12 V bus. Recommend a low-voltage
  MOSFET (<= 20 mohm, Qg <= ~60 nC) once the waveform tests pass.

## Decisions Already Made

- Use ESP32 MCPWM hardware for 150 kHz switching.
- Do not use software loops or `digitalWrite()` for high-frequency waveform
  generation.
- Keep powered bridge operation disabled by default.
- Originally: serial commands instead of automatic startup. Changed at user
  request 2026-10-11 to auto-run at boot (`kAutoRunAtBoot`).
- Keep the relay separate from high-frequency switching.
- Treat the hardware topology as unverified until a schematic and measurements
  prove otherwise.

## Open Hardware Risks

- `[BLOCKER]` Full-bridge diagonal mapping is stated by the user but not yet
  shown by schematic or scope.
- `[BLOCKER]` TLP250 propagation delay (0.5 us max) and part-to-part skew are
  not measured. The 700 ns dead time is a datasheet-derived starting point.
- `[BLOCKER]` TLP250 LED wiring (GPIO sources anode?) is not confirmed. One
  GPIO drives two TLP250 LEDs: ~7-10 mA each, ~20 mA total. That is at the
  ESP32 default drive limit, so use a buffer (74HCT244 or a transistor).
- `[BLOCKER]` High-side TLP250 floating supplies (2 floating + 1 low-side, or
  bootstrap) are not confirmed.
- `[BLOCKER]` GPIO13/GPIO14 are JTAG pins (MTCK/MTMS). Their level during
  reset/boot (before `setup()`) is not firmware-controlled. External pull-downs
  on all three opto inputs are needed, and power-up must be checked on a scope.
- `[BLOCKER]` Gate-to-source voltage for each MOSFET is not confirmed.
- `[BLOCKER]` Dedicated gate-driver circuitry is not confirmed.
- `[BLOCKER]` Fast overcurrent protection is not confirmed.
- `[BLOCKER]` Relay DC contact rating and inrush behavior are not confirmed.
- `[BLOCKER]` No resonant capacitors fitted. Do not fit them at full 12 V
  until overcurrent protection exists (see SS hazard above).
- `[TODO]` No hardware watchdog yet. If firmware hangs while running, MCPWM
  keeps switching.
- `[TODO]` MCPWM hardware fault input (`mcpwm_fault_init` + one-shot trip)
  is not used yet. It is the intended fast overcurrent path.

## Next Recommended Tasks

1. Flash the new firmware. Run relay-off `pwm` and confirm a 6.66 us period
   (150.09 kHz) at GPIO14/GPIO26 and non-overlap of ~0.7 us.
2. Scope power-up and reset: GPIO13/14/26 and TLP250 outputs must stay LOW.
3. Scope TLP250 outputs and every MOSFET gate-to-source. Measure delay and
   skew, then set `kDeadTimeNs` from the measurements.
4. Measure coil L with an LCR meter. Choose SS capacitors for ~145-150 kHz.
5. Add overcurrent sensing into the MCPWM fault input (one-shot trip) and a
   bus fuse before fitting the capacitors.
6. Commission at 2-3 V bus, current-limited, with the receiver present. Tune
   `freq` to just above resonance.
7. Consider replacing IRF840 with a low-voltage MOSFET for the 12 V bus.
8. Only after hardware proof, decide whether to arm `run`.

## Verification Checklist For Future Changes

For documentation-only changes:

- Check that docs agree with `src/main.cpp`.
- No build required unless code or config changed.

For firmware changes:

- Build with `/home/irfan/.platformio/penv/bin/platformio run`.
- Confirm safe boot behavior remains relay OFF and switching inactive.
- Confirm `run` remains locked out unless intentionally armed.
- If waveform behavior changes, update oscilloscope procedure and record the
  expected waveform.

For hardware-enabling changes:

- Require schematic evidence.
- Require oscilloscope evidence at MOSFET gate-to-source, not just ESP32 pins.
- Require a low-energy commissioning plan.
- Document remaining unverified risks in this file and in
  `docs/wpt-controller-design.md`.
