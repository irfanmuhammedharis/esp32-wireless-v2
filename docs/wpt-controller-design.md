# ESP32 150 kHz WPT Transmitter Controller - Design

Last updated: 2026-10-11. Measurements, history, and open items are in
`project-memory.md`; this file describes the intended design and how to
commission it.

## System Overview

```
 12 V supply ──> supply module (IRF840 switch, GPIO13 via opto 1) ──> +V bus
                                                                       │
 ESP32 ── GPIO14 ─> TLP250 2+3 ─> Q1 high side ┐                       │
       ── GPIO26 ─> TLP250 4+5 ─> Q2 low side  ├─ half bridge ─ series C ─ TX coil
       ── GPIO27 <─ IR sensor (LOW = object)   ┘
       ── GPIO25 ─> servo (0 / 45 deg)
       ── GPIO21/22 ─> 16x2 I2C LCD
                                         RX coil (identical) ─> receiver
```

An object in front of the IR sensor turns charging on: the servo goes to
45 deg and the half bridge drives the transmitter coil at ~150 kHz. When the
object leaves, the servo goes back to 0 deg and power is removed.

## Wiring And Signal Mapping

| ESP32 GPIO | Connects to | Firmware role | Notes |
|---|---|---|---|
| GPIO13 | Opto 1 -> supply-module IRF840 gate | Supply on/off ("relay") | Not a 150 kHz output. Default OFF. |
| GPIO14 | TLP250 2, 3 | MCPWM0A, **high side** | Complementary to GPIO26 with dead time. |
| GPIO26 | TLP250 4, 5 | MCPWM0B, **low side** | Inverse of GPIO14 with dead time. |
| GPIO21 | LCD SDA | I2C, backpack at 0x27 | Needs 3.3 V-safe pull-ups or a level shifter. |
| GPIO22 | LCD SCL | I2C | Same as SDA. |
| GPIO27 | IR sensor OUT | Input with pull-up | LOW = IR on = charge. Power the module from 3.3 V. |
| GPIO25 | Servo signal | LEDC 50 Hz | Servo on its own 5 V supply, GND shared. |

Fit external pull-downs (~10 k) on GPIO13/14/26. GPIO13 and GPIO14 are JTAG
pins and are not under firmware control during reset and boot.

## Power Stage: Reference Half Bridge

```
 +V bus ─────┬────────────────┬────────────┐
             │ D              │            │
 TLP250 #1 ──┤G  Q1 IRF840   ═ 470 uF     ═ 1 uF film
 (GPIO14)    │ S               + 100 nF    │
             ├── SW ──[ C ~14 nF film 630 V ]──[ TX COIL ]──┐
             │ D                                            │
 TLP250 #2 ──┤G  Q2 IRF840                                  │
 (GPIO26)    │ S                                            │
 GND ────────┴────────────────┴────────────┴────────────────┘

 TLP250 #1 (high side): VEE = SW, VCC = SW + 15 V
   bootstrap: fast diode (UF4007) from +15 V to VCC, 10 uF + 100 nF VCC to SW
 TLP250 #2 (low side): VEE = GND, VCC = +15 V
 Each gate: 10 ohm series, 10 k gate-to-source
```

- The user reports the bridge is wired this way. Not yet verified by a scope
  or a schematic.
- Paralleled MOSFETs (two high, two low) are fine, each with its own 10 ohm
  gate resistor.
- The series capacitor is mandatory in a half bridge. It blocks the 6 V DC
  midpoint and resonates the coil. C = 1 / ((2*pi*f)^2 * L): about 14.2 nF for
  L = 79 uH. Measure L first.
- IRF840 (0.85-1.5 ohm) wastes power at 12 V. A low-RDS(on) part such as the
  IRFZ44N (17.5 mohm) is recommended.

## Switching Sequence

| Interval | GPIO14 (high) | GPIO26 (low) | Switch node |
|---|---|---|---|
| Half-cycle 1 | ON | OFF | +V bus |
| Dead time | OFF | OFF | Coil current commutates through the body diodes |
| Half-cycle 2 | OFF | ON | GND |
| Dead time | OFF | OFF | Body diodes again |

- MCPWM at 80 MHz: 533 ticks = 6.6625 us period (150.09 kHz).
- Dead time 700 ns (56 x 12.5 ns, probably 712.5 ns in hardware). That leaves
  ~2.62 us of conduction per half-cycle.
- 700 ns comes from the TLP250 0.5 us maximum delay plus IRF840 turn-off. Set
  it from measured gate waveforms once available.
- Operate slightly **above** resonance (inductive load, soft switching). Below
  resonance, the slow IRF840 body-diode recovery causes destructive hard
  switching.

## Supply Module (GPIO13)

The "relay" is a perfboard module: cement resistor, electrolytic, 1 mH/10 A
choke, IRF840 (source to GND, 12 V gate drive, no heatsink), diode, small
electrolytic, second 1 mH choke. That layout is a **boost converter**. Holding
its MOSFET ON (as `run` does) likely shorts 12 V through the choke, and the
IRF840 overheats.

Recommended replacements:
- A P-channel high-side switch (IRF4905, ~20 mohm): source to +12 V, 10 k
  gate-to-source, opto 1 pulls the gate to GND for ON (Vgs = -12 V).
- Or feed the bridge straight from a current-limited bench supply and ignore
  GPIO13.

Use `opto13_test` to identify the module (see `project-memory.md`).

## Firmware Behavior

- Safe boot: GPIO13/14/26 LOW, servo 0 deg, `Idle`.
- Run (IR only, no serial command): PWM starts with GPIO13 OFF, GPIO13 turns ON 20 ms later, then
  `Running` after 50 ms. Requires `kPowerStageTopologyVerified` and
  `kAllowRelayEnergizedRun` (both currently true).
- `stop`: GPIO13 OFF, wait 80 ms, detach and force GPIO14/26 LOW, stop MCPWM.
- `freq <hz>`: 100-200 kHz, Idle only. The legacy driver changes the period
  immediately, which could stretch a cycle while running.
- Start safety: MCPWM is configured with dead time active before the pins are
  routed to it, so the reset-default bypass (A = B) never reaches the gates.
- Fault: any MCPWM API error latches `FaultLocked`, turns GPIO13 and the gates
  OFF, and needs a reset to clear.
- IR control: the only way to turn on. IR on (LOW) -> servo 45 deg -> run;
  IR off -> servo 0 deg -> `stop`. 100 ms debounce, active 2 s after boot.
  The relay-off `pwm` scope command was removed (user request 2026-10-12).
- LCD: state + relay on line 1, frequency + switching on line 2, refreshed
  every 500 ms; shows the fault reason when faulted.

High-frequency switching is done by MCPWM hardware. The Arduino loop only
handles serial commands, IR, the state machine, and the LCD.

## Oscilloscope Test Procedure

1. Disconnect the +V bus from the half bridge. Power only the ESP32 and the
   TLP250 supplies.
2. Reset and confirm GPIO13/14/26 stay LOW through the whole boot.
3. Block the IR sensor (this also turns GPIO13 ON, so the bus must be
   disconnected). At GPIO14 and GPIO26 expect a 6.66 us period, complementary,
   with ~0.7 us where both are LOW. A 6.0 us period means old firmware.
4. Capture the first cycle after IR on in single-shot: both pins must never be
   HIGH together.
5. Probe gate-to-source on Q1 (relative to the switch node, not GND) and on
   Q2. Expect 0 / ~15 V with non-overlap of at least ~200 ns after TLP250
   delays.
6. Send `stop`: GPIO13 OFF, then both gates LOW.

## Commissioning Procedure

1. Pass the scope test above.
2. Resolve the supply module (see above), or bypass it.
3. Measure coil L and fit the series capacitor (polypropylene film, >= 630 V).
4. Use a current-limited bench supply at **2-3 V** with the receiver present.
   At resonance with no receiver, 12 V gives ~5 A rms and ~380 V rms on the
   capacitor.
5. Use `freq` to find resonance (maximum receiver voltage), then settle a few
   kHz above it.
6. Raise the bus voltage in steps while watching current and MOSFET
   temperature.
7. Before full power, add a fuse and fast overcurrent shutdown (MCPWM fault
   input with one-shot trip). Do not rely on GPIO13 as protection.
