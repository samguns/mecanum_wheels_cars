# Implementation Plan: MIT Impedance Control Mode with Deterministic Current Loop

**Branch**: `002-mit-impedance-control` | **Date**: 2026-08-20 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/002-mit-impedance-control/spec.md`

## Summary

Replace the opportunistic FOC loop with a deterministic, PWM-phase-locked control loop whose rate is derived
from a user-configurable current-loop bandwidth, and add MIT-style impedance control as a per-motor motion mode
alongside the permanently retained velocity mode.

The technical approach has three pillars. First, a hardware time base: an MCPWM `on_full` event callback,
decimated by an integer factor, releases a high-priority task pinned to core 1, so the control period is set by
the PWM timer rather than by scheduler slack. Second, a fixed timestep: `Ts` is set explicitly on every PID and
low-pass filter, which SimpleFOC 2.4.0 already supports but the sketch never uses, so jitter can no longer leak
into gain error. Third, an execution-time budget: the present sensor path costs about 240 µs per cycle, of which
100 µs is a hardcoded `delayMicroseconds(50)` inside the vendored SimpleFOC SPI sensor, so reaching the required
1000 Hz default needs a first-party AS5147 reader using one pipelined transfer at 8 MHz.

The honest consequence, established in research and carried into the design: the 100-10000 Hz input range is
accepted in full, but the reachable ceiling is around 1000 Hz, so most of the upper range clamps. That is
exactly the clamp behaviour chosen during clarification, and the ceiling is published from a measured number
rather than assumed.

## Technical Context

**Language/Version**: C++17 (Arduino ESP32 core 3.3.7) for firmware; Python 3.12 for the backend;
TypeScript 5.9 with Vue 3.5 for the UI

**Primary Dependencies**: Simple FOC 2.4.0, ESP-IDF 5.x MCPWM and TWAI drivers, Arduino `Preferences`;
FastAPI, python-socketio, python-can; Vue 3, Vite 7, socket.io-client 4.8

**Storage**: ESP32 NVS via `Preferences`, namespace `robot_config`, key `cfg`, holding the `RobotConfig` blob.
`CONFIG_VERSION` goes 1 → 2 with a migration that preserves existing calibration data

**Testing**: `arduino-cli compile --fqbn esp32:esp32:esp32` as the firmware gate, plus scope-verified loop
timing and the attended bench scenarios in [quickstart.md](./quickstart.md); `npm run type-check` for the UI.
No automated test harness exists in this repository and this feature does not introduce one

**Target Platform**: Classic ESP32 (dual core, 240 MHz) motor controllers on a 1 Mbit/s CAN bus, commanded by a
Jetson Xavier running the backend and serving the operator UI

**Project Type**: Embedded firmware plus a Python service plus a Vue single-page UI, three layers changed
together because the CAN contract spans them

**Performance Goals**: Control cycle jitter within 5% of nominal for 99.9% of cycles with zero dropped cycles;
measured rate within 1% of nominal; 200 Hz paired-command rate with a 50 ms timeout; motion command latency bounded
by the approximately 27% bus utilisation budget and remaining below the 50% contract ceiling

**Constraints**: MCPWM carrier capped at 50 kHz by a SimpleFOC constant; `adcRead` costs ~9 µs per channel and
four channels are needed per cycle; inline current sensing has no PWM-synchronised sampling on ESP32; the
vendored SimpleFOC install lives outside the repository and must not be edited; per-motor current limit 3.0 A on
a 12 V bus

**Scale/Scope**: 2 controllers, 4 motors, 2 motion modes, 11 new CAN frame families, ~2120 frames/s;
one firmware sketch, one backend module, one Vue component

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### I. Safety-Critical Motion First

**Status: PASS (with mandatory design obligations)**

This feature changes motor direction semantics, control mode, current limits, and CAN command encoding, so it is
safety-critical in full. Obligations discharged in the design:

- Deterministic zero-output paths are enumerated and individually tested: disarm, emergency stop, command
  timeout, mode change, calibration entry, sustained overrun. See quickstart S3.
- Enabling stiffness on an armed wheel cannot use a delayed backend position: a flag in the matched pair makes
  firmware capture the same-cycle measured position, and `0x1E0` confirms the applied target before holding.
- Forward, reverse, strafe, and rotation behaviour is defined by the unchanged mecanum mixer and verified in
  both modes (SC-015, SC-010, quickstart S7 and S8).
- Impedance control introduces a hazard velocity control lacked: a stale position target with non-zero
  stiffness pushes indefinitely. Mitigated by the 50 ms command timeout, 1.0 rad position-error saturation,
  pre-existing current and output-voltage limits, plus a new fail-closed bus-voltage protection path built on
  the existing monitor. Every limiting cause is latched for communications reporting and verified independently.
- Bench-safe path (compile, disarmed timing runs) and hardware path (S3 through S9) are both specified.

### II. Protocol Compatibility Is a Hard Contract

**Status: PASS**

- All identifiers, byte layouts, scaling, signedness, and left/right mapping are documented in
  [contracts/can-protocol.md](./contracts/can-protocol.md) in the same change that introduces them.
- Backward compatibility is structural but configuration-dependent: velocity mode is retained permanently and
  is the fresh/invalid-config default. Accepted per-motor modes persist, so S8 first stores velocity for all four
  motors, power-cycles, and then proves an unmodified sender works.
- Firmware, backend, and UI are all in scope and change together (FR-042).
- The impedance command is an atomic big-endian two-frame pair: signed 32-bit milliradian absolute position in
  one half and the remaining four MIT-style dynamics fields in the other. Matching sequence numbers prevent
  partial application; a capture flag and applied-target status close the stiffness-transition race. The legacy
  velocity frame remains little-endian, and the contract forbids sharing helpers.

### III. Validate at the Boundary You Changed

**Status: PASS**

| Change | Narrowest falsifying check |
|---|---|
| Frame packing | Pack/unpack round-trip against the documented byte layout |
| Impedance law | Torque computed from known inputs, checked before energising |
| Loop determinism | Scope on an instrumented GPIO plus the `T` report |
| Bandwidth derivation | `B` report showing rate, carrier, decimation, integer ratio |
| Config migration | Power cycle with a version-1 blob, calibration intact |
| Firmware overall | `arduino-cli compile` |
| Backend mixer/encoding | Focused Python run asserting frame bytes |
| UI | `npm run type-check` plus manual readback |

### IV. Configuration Must Not Be Hard-Coded into Behavior

**Status: PASS**

- Bandwidth is persisted configuration, not a literal, replacing the hardcoded `100.0f` at sketch line 300.
- Bus-voltage protection thresholds are persisted millivolt configuration with disarmed-only serial writes;
  provisional 9/14 V defaults and every other provisional safety value are reconciled before the definitive
  acceptance tests. Any later value change invalidates and repeats every affected test before release.
- Impedance ranges, the saturation limit, the timeout, and the sampling multiple become named constants shared
  by contract and code rather than scattered literals.
- No new secrets or environment-specific endpoints. The backend keeps reading its CAN interface from existing
  configuration.
- Opportunistic cleanup in scope: `driver2.pwm_frequency` is currently never set (sketch line 779 sets only
  `driver1`), relying on a library default. Both drivers will be set explicitly from the derived carrier.

### V. Preserve Clear Ownership Boundaries

**Status: PASS (one justified addition, see Complexity Tracking)**

- Firmware owns actuation and timing; the backend owns mixing and transport; the UI owns interaction. Mixing
  stays in the backend and is unchanged.
- `src/MPU6050` and `src/I2Cdev` are untouched.
- The vendored SimpleFOC install is **not** modified. The 50 µs SPI delay is bypassed by adding a first-party
  sensor reader in the repository, which is the constitution-preferred direction. That new component is the
  single justified complexity item below.

### Post-Phase 1 re-check

**Status: PASS.** The design added no new violations. Two items were tightened during design rather than
deferred: the mode-mismatch rejection rule (a velocity frame arriving at an impedance-mode motor is rejected
rather than reinterpreted, which closes a byte-aliasing hazard between two live frame formats), and the
backend's fixed-rate transmit loop (event-driven sending would have made the 50 ms failsafe depend on operator
pointer movement).

## Project Structure

### Documentation (this feature)

```text
specs/002-mit-impedance-control/
├── plan.md                        # This file
├── spec.md                        # Feature specification with 5 recorded clarifications
├── research.md                    # Phase 0: 11 decisions, 5 measurement gates
├── data-model.md                  # Phase 1: config, timing, runtime state, transitions
├── quickstart.md                  # Phase 1: 10 validation scenarios, safety-ordered
├── contracts/
│   ├── can-protocol.md            # Atomic pair layout, accumulated-position/status frames, ranges, bus budget
│   ├── serial-console.md          # B / M / T / I commands, bandwidth write authority
│   └── socketio-events.md         # New mode/gain events, controller status readback
├── checklists/
│   └── requirements.md            # Spec quality checklist, 16/16 passing
└── tasks.md                       # Phase 2 output ($speckit-tasks — NOT created here)
```

### Source Code (repository root)

```text
v13_macnum_wheel_car.ino           # Firmware: impedance law, deterministic loop, config,
                                   #   CAN handling, serial commands (single-sketch project)
imu_helpers.{h,cpp}                # Unchanged
src/MPU6050/, src/I2Cdev/          # Vendored, MUST NOT be modified

jetson_xavier/
├── backend/
│   ├── socketio_server.py         # CanPublisher: impedance frame builder, 200 Hz TX loop,
│   │                              #   mode/gain events, 0x1A0-0x1F0 status decode
│   └── requirements.txt           # Unchanged
└── webUI/src/
    └── App.vue                    # Mode selector, gain inputs, controller status panel
```

**Structure Decision**: The existing layout is kept unchanged. Firmware remains a single Arduino sketch at the
repository root, because that is what the Arduino build expects and splitting it would be a build-system change
unrelated to this feature. The one new firmware component is a first-party AS5147 reader; it is added as a
sketch-local header/implementation pair rather than a new directory tree, matching how `imu_helpers` already
sits beside the sketch. The backend stays a single module and the UI stays a single component, since neither
grows enough here to justify restructuring.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| A first-party AS5147 SPI reader duplicating functionality that SimpleFOC's `MagneticSensorSPI` already provides | `MagneticSensorSPI::read()` contains a hardcoded `delayMicroseconds(50)` on ESP32, costing 100 µs of the ~240 µs cycle budget for two encoders. Without removing it the loop cannot exceed ~4 kHz, which caps bandwidth near 400 Hz and makes the required 1000 Hz default unreachable. | Editing the installed SimpleFOC was rejected: the library lives outside the repository at `C:\Users\guns_\Documents\Arduino\libraries\Simple_FOC`, so the edit would be invisible to version control, lost on library update, and unreproducible for any other build host. Raising only `clock_speed` was rejected as insufficient: it removes ~28 µs but leaves the 50 µs delay, still capping the loop near 9 kHz combined with other costs. Subclassing was rejected because the delay lives in the base-class `read()` that the subclass would have to bypass anyway. |
| Two live motion command formats with different endianness on the same bus | Clarification fixed velocity mode as permanently supported. The atomic impedance pair uses big-endian numeric fields while the existing velocity frame is little-endian. | Normalising the impedance pair to little-endian would preserve neither the selected wire contract nor existing MIT-style dynamics tooling. Distinct identifiers, per-motor mode gating, matching sequence numbers, and separate packing helpers contain the risk. |
| Two CAN frames per motor for one impedance command | A signed 32-bit milliradian absolute position is required to represent continuous rotation and the 100-revolution safety test without sacrificing dynamics resolution. | A rolling origin retained lower traffic but introduced synchronization hazards; a relative-error command abandoned the specified absolute-position semantics. At ~27% estimated worst-case utilisation, the atomic pair remains below the 50% bus ceiling. |

## Phase 2 preview (not executed here)

`$speckit-tasks` should sequence work so that determinism is proven before any torque is produced:

1. Config migration and bandwidth plumbing, verified disarmed.
2. First-party AS5147 reader plus timing instrumentation, verified by scope and the `T` report. **This is the
   gate that publishes the real bandwidth ceiling** (research M1-M3, quickstart S1).
3. Deterministic trigger: MCPWM callback, decimation, high-priority task, fixed `Ts`.
4. Impedance law with saturation, explicit current/output-voltage/bus-voltage limiting, latched limit causes,
   protocol-fault reporting, and the timeout failsafe.
5. Atomic position-and-dynamics CAN pair handling, controller-local hold capture, mode gating, measured/applied
   position readback, and new status frames.
6. Backend transmit loop and frame builders.
7. UI controls and status panel.
8. Bench validation per quickstart, then the hardware verification note.

Steps 2 and 3 carry the schedule risk. If the measured ceiling lands below 1000 Hz, the active bandwidth is
clamped and reported while the requested default remains 1000 Hz; the sampling multiple is never reduced below
ten. ADC1 DMA sampling is the larger route to a higher ceiling and is deliberately out of scope.
