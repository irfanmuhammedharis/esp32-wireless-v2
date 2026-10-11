# ESP32 150 kHz WPT Transmitter Controller

## Feasibility

This firmware can generate two non-overlapping 150 kHz command signals on a
classic ESP32 using MCPWM. The current hardware is not fully verified from the
information available here, so powered bridge operation is locked out in code.

Open hardware items before energizing the DC bus:

Hardware as reported by the user (corrected 2026-10-11): 12 V bus, IRF840
HALF bridge, TLP250 gate drivers, GPIO14 -> high side, GPIO26 -> low side,
coil + series capacitor from the switch node to GND. The complementary
dead-time waveform is the correct drive for this. Coils are
1.0 mm wire, 20 turns, 10 cm diameter, with an identical receiver. No resonant
capacitor is fitted yet. See `project-memory.md` for the full calculations.

- Confirm the diagonal-pair mapping on the schematic and with a scope.
- TLP250: tpLH/tpHL are 0.15 us typ, 0.5 us max. Measure the real delay and
  part-to-part skew at 150 kHz. Dead time must exceed worst skew plus IRF840
  turn-off (~69 ns).
- Confirm each high-side TLP250 has its own floating 15 V supply (or a
  bootstrap) and that every MOSFET sees ~15 V gate-to-source.
- TLP250 is non-inverting. Confirm the GPIO sources current into the LED
  anode. One GPIO drives two LEDs (~20 mA total), so a buffer is recommended.
- Fit external pull-downs on GPIO13/14/26 opto inputs. GPIO13/14 are JTAG pins
  and are not firmware-controlled during boot.
- Confirm current limiting, fast overcurrent shutdown, DC bus fuse, thermal
  protection, and snubbing/clamping are present.
- Confirm relay contact DC voltage/current rating and inrush rating.
- Fit series (SS) capacitors on both coils: C ~14.2 nF for L ~79 uH, using
  polypropylene film rated >= 630 V DC. Measure L first:
  `f = 1 / (2*pi*sqrt(L*C))`.
- SS warning: transmitter current rises when the receiver is removed. At
  resonance with no receiver, a 12 V bus gives ~5.7 A rms and ~420 V rms on
  the capacitor. Overcurrent protection is required before full bus voltage.

## Wiring And Signal Mapping

| ESP32 GPIO | Optocoupler | Firmware role | Notes |
|---|---:|---|---|
| GPIO13 | 1 | Relay enable | Not a 150 kHz output. Defaults OFF. |
| GPIO14 | 2, 3 (TLP250) | MCPWM0A command A | Diagonal pair A (user-stated). |
| GPIO26 | 4, 5 (TLP250) | MCPWM0B command B | Diagonal pair B, complement of A. |
| GPIO21 | - | LCD I2C SDA | 16x2 LCD, PCF8574 backpack. Status display only. |
| GPIO22 | - | LCD I2C SCL | Needs 3.3 V-safe pull-ups or a level shifter. |

GPIO14 and GPIO26 must not be treated as safe MOSFET gate signals until the
opto and gate-driver stages are verified at the MOSFET gates with a scope.

## Switching Sequence

For a verified full bridge using diagonal-pair drive:

| Interval | Command A, GPIO14 | Command B, GPIO26 | Intended bridge action |
|---|---|---|---|
| First half-cycle | Active | Inactive | Diagonal pair A applies one tank polarity. |
| Dead time | Inactive | Inactive | Both pairs off; prevents shoot-through. |
| Second half-cycle | Inactive | Active | Diagonal pair B applies opposite tank polarity. |
| Dead time | Inactive | Inactive | Both pairs off; prevents shoot-through. |

MCPWM runs at 80 MHz, so a 150 kHz request gives 533 ticks = 6.6625 us
(150.09 kHz). Dead time is 700 ns (56 x 12.5 ns, probably 712.5 ns in
hardware), which leaves ~2.62 us of conduction per half-cycle. The 700 ns value
is derived from the TLP250 0.5 us max delay. Replace it after measuring the
actual opto, gate-driver, and MOSFET timing.

Operate slightly above resonance (inductive load) so the IRF840 body diodes
commutate softly. Below resonance, their slow reverse recovery causes hard,
destructive switching.

If GPIO14/GPIO26 instead drive the two legs of a half bridge or if the opto
outputs invert, this sequence must be changed before enabling the relay.

## Firmware Behavior

- Safe boot: relay OFF, GPIO14 LOW, GPIO26 LOW.
- `pwm`: starts complementary MCPWM on GPIO14/GPIO26 with relay OFF. Idle
  only.
- `freq <hz>`: sets 100000..200000 Hz while Idle; applied on next start.
  Changing frequency while running is refused (driver updates the period
  immediately and can stretch a cycle with one output stuck ON).
- Start: MCPWM is configured and running with dead time active while the pins
  are plain GPIO LOW. Pins are routed to MCPWM last, so the bypass window
  where A and B are identical never reaches the gates.
- Fault: any MCPWM API failure latches `FaultLocked`, which turns the relay
  OFF and gates OFF immediately. Only a reset clears it.
- `run`: refused by default until these constants in `src/main.cpp` are true:
  `kPowerStageTopologyVerified` and `kAllowRelayEnergizedRun`.
- Powered start sequence after verification: relay OFF, PWM starts, wait for
  PWM stabilization, then relay ON, then wait for relay settling.
- Stop sequence: relay OFF first, wait for contact dropout, then detach
  GPIO14/GPIO26 from MCPWM and force them LOW, then stop the MCPWM timer.

- LCD: line 1 state + relay, line 2 actual frequency + switching; fault
  reason on fault. Refreshed every 500 ms (see project-memory).

High-frequency generation uses ESP32 MCPWM hardware, not `digitalWrite()` or a
software loop. The Arduino loop only handles serial commands and state changes.

## Oscilloscope Test Procedure

1. Disconnect or disable DC bus power to the MOSFET bridge. Leave the relay
   output unloaded if possible.
2. Power only the ESP32 and isolated driver/control supplies needed for logic
   probing.
3. Reset the ESP32 and confirm GPIO13 is LOW and the relay is OFF.
4. Send `pwm` at 115200 baud.
5. Probe GPIO14 and GPIO26 at the ESP32 pins: expect a 6.66 us period
   (150.09 kHz), complementary, with ~0.7 us where both are LOW at each edge.
   A 6.0 us period means old firmware (166.7 kHz).
5a. Capture GPIO14 and GPIO26 together in single-shot mode, triggered on the
   first edge after `pwm`. They must never be HIGH at the same time, including
   the first cycle.
6. Probe optocoupler outputs: confirm polarity, delay, rise/fall time, and that
   pulse widths are still valid at 150 kHz.
7. Probe each MOSFET gate-to-source directly, not gate-to-ground for high-side
   MOSFETs. Confirm no two MOSFETs in the same leg are ON together.
8. Send `stop` and confirm relay OFF, MCPWM stopped, GPIO14/GPIO26 LOW.
9. Repeat with driver supply tolerances and expected temperature range if this
   is more than a bench prototype.

## Commissioning Procedure

1. Verify the schematic against the open hardware items above.
2. Bring up logic only with no bridge bus power.
3. Use a current-limited bench supply for the DC bus at 2-3 V first, never the
   full 12 V, especially once the SS capacitors are fitted.
4. Add a fast fuse or electronic current limit in series with the bus.
5. Test without the resonant coil first if the bridge allows a safe dummy load.
6. Test with the resonant tank at very low bus voltage, with the receiver
   present, and watch tank voltage, current, MOSFET temperature, and driver
   waveforms. Use `freq` to find resonance, then settle slightly above it.
7. Increase bus voltage in small steps while recording current and thermal data.
8. Add firmware fault inputs before full-power operation: overcurrent,
   overtemperature, bus undervoltage/overvoltage, and driver fault if available.
9. Do not use relay-only shutdown as the primary protection. Use a hardware
   fast-disable path that removes gate drive on overcurrent.
