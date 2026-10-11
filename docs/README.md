# ESP32 WPT Controller Documentation

This documentation is the handoff package for future AI agents and humans
working on the ESP32 150 kHz wireless power transfer transmitter controller.

## Reading Order For Future Agents

1. `../AGENTS.md` - standing rules, safety constraints, build command, and
   agent workflow.
2. `project-memory.md` - current project memory, known decisions, open risks,
   and next actions.
3. `wpt-controller-design.md` - electrical feasibility, switching sequence,
   oscilloscope test plan, and commissioning procedure.
4. `../src/main.cpp` - implemented firmware.
5. `../platformio.ini` - PlatformIO target and framework.

## Project Summary

The firmware targets an ESP32 Dev Module running Arduino through PlatformIO. It
controls a wireless power transfer transmitter prototype:

- GPIO13 controls the main-circuit relay through Optocoupler 1.
- GPIO14 drives Optocouplers 2 and 3 as switching command A.
- GPIO26 drives Optocouplers 4 and 5 as switching command B.
- ESP32 MCPWM generates the 150 kHz switching waveform.
- Powered relay operation is locked out until hardware topology and gate-drive
  polarity are verified.

## Document Index

| File | Purpose |
|---|---|
| `../AGENTS.md` | Instructions future AI agents should follow before editing. |
| `../.agents/project-memory.md` | Pointer for agents that inspect `.agents/` first. |
| `project-memory.md` | Persistent memory: current state, decisions, risks, and next tasks. |
| `wpt-controller-design.md` | Hardware/firmware design note and commissioning procedure. |

## Status

Mode: brownfield firmware prototype.
Last updated: 2026-10-11.

Legend:

- `[CONFIRMED]`: observed in code or build output.
- `[ASSUMPTION]`: chosen safe default that still needs user or hardware proof.
- `[TODO]`: open work item or question.
- `[BLOCKER]`: must be resolved before powered operation.
