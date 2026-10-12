# Meta-Prompt — Generate `docs/code-explanation.html`

Use this meta-prompt with a coding agent that has this repository open to
(re)generate the HTML "Code Explanation & Working Document". It is written to
be self-contained and re-runnable.

---

## Role

You are a senior embedded-systems engineer and technical writer. You document
power-electronics firmware precisely, you never overclaim what has been
verified, and you show every calculation used.

## Objective

Produce a single, self-contained HTML document that explains how the firmware
in `src/main.cpp` works and how the hardware operates, for the current hardware
revision in which:

- the GPIO13-controlled "relay" is an **actual electromechanical relay**
  (5 V coil) that switches the 12 V feed to the power stage — the earlier
  IRF840-based perfboard "supply module" is no longer the relay; and
- a **7805 linear regulator** derives the relay's 5 V operating voltage from
  the 12 V rail.

## Read first (context)

1. `AGENTS.md` — standing rules, pin map, safety constraints.
2. `docs/project-memory.md` — current state, decisions, open risks.
3. `docs/wpt-controller-design.md` — electrical assumptions, sequences.
4. `src/main.cpp` — the implemented firmware being explained.
5. `platformio.ini` — toolchain pins and build settings.

## Required content

1. **Code explanation** — module-by-module walkthrough of `src/main.cpp`:
   pin map, MCPWM clocking, dead time, safe GPIO configuration, waveform
   start/stop ordering, fault latch, state machine, IR + servo control, LCD,
   serial console, `setup()` / `loop()`.
2. **Calculations** — at least:
   - MCPWM: 80 MHz timer resolution → 533 ticks → 150.09 kHz actual,
     ~0.28 kHz tuning step, 16-bit period register limit.
   - Dead time: 700 ns → 56 group-clock ticks → ~712.5 ns, leaving ~2.62 µs
     conduction per half cycle.
   - Relay sequence timing budget (20 ms / 50 ms / 80 ms) vs typical relay
     operate/release/contact-bounce times.
   - Servo PWM: 50 Hz, 16-bit, 500–2500 µs → 1638–8191 counts; 45° = 1000 µs.
   - 7805: P = (12 − 5) × I_load, example 71 mA coil → ~0.5 W and the
     resulting junction-temperature rise.
   - Optocoupler LED resistor, CTR budget, and relay-driver base current.
3. **Algorithm** — controller state machine (all 7 states), run/stop/fault
   sequences as timelines + pseudocode, IR debounce logic, serial command
   parsing, and the cooperative main loop.
4. **Working document** — architecture diagrams (signal path and power path
   including relay + 7805), wiring/pin table, serial command reference,
   safety notes and verification status.

## Constraints

- Documentation only: do **NOT** modify `src/main.cpp` or any firmware file.
- Do **NOT** document test files or the `opto13_test` environment.
- Single HTML file with embedded CSS; no external CDNs, fonts, or JS —
  it must render offline.
- Use short, verbatim (HTML-escaped) code excerpts from `src/main.cpp`
  where they aid understanding.
- Mark unverified/pending items clearly — nothing has been scope-measured.
- Preserve the safety language of `AGENTS.md` (safe default state, arm flags,
  "relay shutdown is not fast protection").

## Output

- `docs/code-explanation.html` (overwriting an existing copy is fine).

## Success criteria

- Every hardware statement matches the relay + 7805 revision.
- Every number matches the constants in `src/main.cpp` or shows its derivation.
- A new team member could wire the relay/7805 control path, follow the state
  machine timing, and re-derive the MCPWM frequency math from the document
  alone.
