# AGENTS.md - ESP32 WPT Controller Agent Manual

This project controls a wireless power transfer transmitter prototype. Treat it
as power electronics firmware, not a normal Arduino demo. A wrong change can
turn on MOSFETs incorrectly and destroy hardware.

## Read First

Before changing code, read these files in order:

1. `docs/README.md`
2. `docs/project-memory.md`
3. `docs/wpt-controller-design.md`
4. `src/main.cpp`
5. `platformio.ini`

If your task touches ESP32, Arduino, PlatformIO, MCPWM, or ESP-IDF APIs, fetch
current official documentation first. API details matter here.

## Project Facts

- Board target: `esp32dev` in PlatformIO.
- Framework: Arduino on ESP32, using ESP-IDF driver APIs where needed.
- Current firmware entry point: `src/main.cpp`.
- GPIO13 controls only the relay through Optocoupler 1.
- GPIO14 is switching command A through Optocouplers 2 and 3.
- GPIO26 is switching command B through Optocouplers 4 and 5.
- GPIO21/GPIO22 are I2C SDA/SCL for a 16x2 status LCD (LiquidCrystal_I2C,
  `updateLcd()` in `src/main.cpp`). The user wants this kept simple. The
  display is status only and must never gate control.
- GPIO27 is the IR sensor input (LOW = detecting) and GPIO25 is the servo
  signal (LEDC channel 0, 50 Hz).
- Switching target: 150 kHz.
- Waveform generation must use ESP32 MCPWM hardware. Do not generate the 150 kHz
  waveform with `digitalWrite()`, `delayMicroseconds()`, timer polling, or a busy
  loop.

## Safety Rules

- Default safe state is relay OFF and GPIO14/GPIO26 inactive/LOW.
- Do not enable relay-powered operation unless the schematic, MOSFET topology,
  optocoupler polarity, and gate-to-source waveforms are verified.
- Leave `kPowerStageTopologyVerified` and `kAllowRelayEnergizedRun` false unless
  the user provides measurement evidence or explicitly asks to arm powered run.
  Status: armed (true) again 2026-10-11 after the user reported rewiring to
  the reference half bridge (not scope-verified). See `docs/project-memory.md`.
- Do not assume GPIO14 and GPIO26 are safe opposing bridge commands. Confirm the
  actual MOSFET topology first.
- Dead time is not a cosmetic setting. It must be based on measured
  optocoupler, gate-driver, and MOSFET timing. Its unit is the MCPWM *group*
  clock, so changing `kMcpwmGroupResolutionHz` changes dead-time units.
- MCPWM signals must only be routed to GPIO14/GPIO26 after dead time is
  enabled, and the pins must be detached before MCPWM is stopped. Do not
  reorder `startSwitchingOutputs()` / `stopSwitchingOutputs()`.
- Do not change frequency on a running MCPWM timer with the legacy driver.
- Relay shutdown alone is not fast protection. Full-power operation needs
  hardware fault protection, such as overcurrent gate-drive disable.

## Build And Verify

Preferred build command:

```bash
/home/irfan/.platformio/penv/bin/platformio run
```

The plain `pio` command may not be on `PATH`. In the sandbox, PlatformIO may
fail when it tries to write lock/cache files under `/home/irfan/.platformio`;
rerun with approval/escalation if needed.

After code changes:

1. Run the PlatformIO build.
2. Review any compiler warnings.
3. If hardware behavior changed, update `docs/project-memory.md` and
   `docs/wpt-controller-design.md`.
4. State clearly what was not verified on physical hardware.

## Firmware Interaction

Serial baud: 115200.

Commands currently implemented:

- `help`: print command help.
- `status`: print controller state, relay state, switching state, frequency,
  dead time, and verification gates.
- No manual turn-on command (user request 2026-10-12): the IR sensor is the
  only way to start. IR on (LOW) -> servo 45 deg + powered run, IR off ->
  servo 0 deg + stop (acts 2 s after boot and on each IR change). The run
  is still refused unless the compile-time verification gates are true.
- `freq <hz>`: set switching frequency 100000..200000 Hz, Idle only.
- `stop`: turn relay OFF, wait for dropout, then stop PWM and force switch pins
  LOW.

## Documentation Duties

When making a meaningful change, keep these in sync:

- `docs/project-memory.md`: current state, decisions, next tasks, test results.
- `docs/wpt-controller-design.md`: electrical assumptions, commissioning steps,
  switching sequence, and safety warnings.
- This `AGENTS.md`: standing instructions if workflow or safety rules change.

## Do Not Do

- Do not remove the powered-run lockout casually.
- Do not change pin assignments without updating every doc and verifying ESP32
  pin capability.
- Do not claim the circuit is verified from code alone.
- Do not add Wi-Fi, BLE, OTA, or unrelated features unless the user asks.
- Do not silently convert this to another framework or ESP32 variant.
