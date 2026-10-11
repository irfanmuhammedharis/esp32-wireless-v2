# Agent Project Memory Pointer

This project's durable memory is maintained in:

- `../AGENTS.md`
- `../docs/project-memory.md`
- `../docs/wpt-controller-design.md`

Read those files before editing firmware. The most important current safety
fact is that powered relay operation is intentionally locked out in
`src/main.cpp` until the MOSFET topology, optocoupler polarity, gate-driver
behavior, and gate-to-source waveforms are verified.
