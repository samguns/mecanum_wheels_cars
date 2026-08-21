# Implementation Plan: V13 Configurator

**Branch**: `003-v13-configurator` | **Date**: 2026-08-21 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/003-v13-configurator/spec.md`

## Summary

Build a standalone Tauri 2 desktop application with a Vue 3 frontend that connects directly to one controller's
serial port and replaces console typing for motor calibration, configuration and diagnostics.

The technical approach turns on one discovery: **the firmware console is prose, and prose cannot safely be a
machine interface.** Values arrive split across lines, refusals are free text, and internal spacing varies, so a
scraping configurator would eventually misreport a motor's electrical parameters after an innocuous firmware
reword. The plan therefore adds a structured, versioned, single-line record format to the firmware, emitted
alongside the existing human output, and the configurator consumes only that. Parsing lives in one pure Rust
function so the feature's highest-consequence failure is unit-testable on a laptop.

A second discovery makes a small firmware change mandatory rather than optional. Calibration stages block for
seconds inside the command handler, and the only in-stage abort polls the CAN bus. Because the chosen connection
model is direct serial with no vehicle bus required, a bench session today has **no software way to stop an
energised calibration stage.** The spec requires an always-reachable stop, so the firmware must also accept a
serial abort byte at every point it already checks for a CAN abort.

## Technical Context

**Language/Version**: TypeScript 5.9 with Vue 3.5 (frontend), Rust 1.70+ (Tauri core), C++17 on the Arduino ESP32
core 3.3.7 (firmware additions)

**Primary Dependencies**: Tauri 2, `tauri-plugin-serialplugin` 2.22 for native port access and control signals,
Vite 7, Pinia; Vitest for frontend logic and `cargo test` for the parser. Firmware adds no new library.

**Storage**: Local application data on the technician's machine for app settings, operator profile and an
append-only change log. On the controller, nothing new is persisted; every displayed value already exists in
`RobotConfig`, so no `CONFIG_VERSION` bump is needed.

**Testing**: `cargo test` for the record parser and session state machine, `npm run type-check` and Vitest for
frontend logic, `arduino-cli compile --fqbn esp32:esp32:esp32 .` for firmware, and the attended bench scenarios
in [quickstart.md](./quickstart.md). This is the first feature in the repository with a genuinely automatable
core, and the plan deliberately uses that.

**Target Platform**: Windows, macOS and Linux desktop, connected by USB serial to one ESP32 controller. No
network, no Jetson, and no CAN bus required.

**Project Type**: Standalone desktop application plus a firmware protocol addition. Two layers change together.

**Performance Goals**: Telemetry refresh at 200 ms by default with a firmware-enforced floor; staleness visible
within 2 s of updates stopping; command acknowledgement within 2 s, or 60 s for a calibration stage; no new
control-loop overruns while telemetry streams.

**Constraints**: The console serves one request at a time and has no request identity, so requests must be
serialised and tagged. Calibration stages block the communications context, so progress must be volunteered by
the firmware rather than polled. Opening a serial port resets most ESP32 boards through DTR and RTS unless the
control signals are managed. The configurator must offer no motion control at all.

**Scale/Scope**: One controller and its two motors per session; a four-wheel vehicle is two sequential
connections. Roughly 10 record types, 4 writable settings, 2 calibration stages, 2 application screens matching
the reference design's sidebar.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

### I. Safety-Critical Motion First

**Status: PASS, and this gate drove the design**

The configurator can energise a motor, so it is safety-critical in full.

- The in-stage abort gap is the headline finding, not a footnote. Research D2 establishes that a bench session
  currently cannot stop an energised stage, and the serial abort byte closes it. Quickstart S6 is a dedicated
  release gate for exactly this, to be run before any longer calibration is trusted.
- Deterministic zero-output paths are preserved by routing every abort through the firmware's existing
  `failCalibration()`, so the disarm behaviour is identical whichever path triggers.
- A powered procedure cannot start without a confirmation, and that is enforced structurally by an intent token
  in the app's internal interface rather than by UI convention, so a future UI change cannot bypass it.
- Timeouts are distinguished from refusals throughout: a refusal means the controller decided, a timeout means
  the outcome is unknown and the operator must re-read.
- The configurator offers no motion control, verified by inspection in quickstart S10.

### II. Protocol Compatibility Is a Hard Contract

**Status: PASS**

This feature promotes the serial console to a machine-to-machine boundary, which brings it fully under this
principle for the first time.

- The schema is versioned, documented in [`contracts/serial-protocol.md`](./contracts/serial-protocol.md), and
  firmware and configurator change together in the same change.
- Additive changes do not bump the version; removals, renames and unit changes do. A configurator facing an
  unrecognised version refuses to interpret rather than guessing.
- The existing human prose is unchanged, so features 001 and 002 keep their bench workflow. The `#V13` prefix
  guarantees the two streams cannot be confused, in either direction.
- Backward compatibility is real, not incidental: a human at the console sees identical behaviour, and untagged
  commands still work so a person can type alongside the tool.

### III. Validate at the Boundary You Changed

**Status: PASS**

| Change | Narrowest falsifying check |
|---|---|
| Record parsing | `cargo test`, including firmware prose that must be ignored |
| Session and calibration state machines | `cargo test` on pure transitions |
| Frontend logic | `npm run type-check` plus Vitest |
| Firmware record emission | `arduino-cli compile`, then a console capture diffed against the contract |
| Serial abort | Quickstart S6 on hardware, 10 trials per stage per motor |
| Control-loop impact | `T` report shows no new overruns while telemetry streams |

### IV. Configuration Must Not Be Hard-Coded into Behavior

**Status: PASS**

Port, baud, telemetry period and operator profile are settings, not literals. No credentials are stored because
no authentication exists. The app never auto-connects without an explicit action, so a remembered port is a
convenience rather than a behaviour.

### V. Preserve Clear Ownership Boundaries

**Status: PASS with a governance note**

- The firmware remains the sole authority on actuation and on every safety rule. The configurator reproduces no
  guard of its own; where it can predict a refusal it explains it, and the controller still decides.
- Measured values are read-only in the configurator, so the firmware's measure-then-confirm model is intact.
- No edits to `src/MPU6050`, `src/I2Cdev`, or the vendored SimpleFOC install.
- The existing Jetson backend and operator UI are untouched.

### Constitution gate: CLEARED by amendment 1.1.0

**Status: PASS.**

The Runtime and Deployment Constraints previously enumerated four supported components and required all changes
to preserve that stack. A standalone desktop application was a fifth component and not in that list.

An earlier draft of this plan resolved the conflict by reading the MUST as constraining boundaries rather than
forbidding new components. That was a reinterpretation of a normative rule, which the constitution's own
governance section does not permit. The conflict was therefore raised as a blocking prerequisite instead.

**Resolved on 2026-08-21 by constitution amendment 1.1.0**, which adds the desktop configurator to the supported
stack and, importantly, does not merely permit it. The amendment also adds a new **Commissioning Tool
Constraints** section binding any tool that reaches the actuators directly, and a Change Gates entry for
desktop-app validation. The amendment is MINOR because it adds a component and rules rather than weakening any
safety, protocol or validation requirement.

Five of those new constraints govern this feature directly, and this plan already satisfies all five:

| Constitution constraint (1.1.0) | Where this plan satisfies it |
|---|---|
| No control capable of commanding vehicle motion | FR-028, task T068 audit, SC-010 |
| Must not duplicate or override a firmware safety rule | FR-016 and FR-020 as reconciled, task T065 |
| Stop path effective whenever a motor can be energised, hardware verified | FR-015, tasks T019, T020, T027, quickstart S6a and S6b |
| Machine-readable interface versioned, documented, changed with the firmware | `contracts/serial-protocol.md`, research D1 |
| Vehicle fully operable with the tool absent | FR-032a, SC-014, task T109 |

The amendment also formalises a rule this feature discovered the hard way: a tool MUST NOT depend on parsing
output intended for human readers. That is exactly the failure mode research D1 identified in the existing
console output.

### Cross-feature dependency: feature 002

Feature 002 is partially delivered (42 of 91 tasks). Two firmware commands this feature needs do not exist yet:

| Needed | Purpose here | Blocked on |
|---|---|---|
| `V`, `V<min>,<max>` | Writing the bus-voltage protection window | Feature 002 task T036 |
| `M`, `M1I`/`M1V`/`M2I`/`M2V` | Writing the per-motor control mode | Feature 002 task T057 |

Both are needed only for **writing** two of the four writable settings, so they gate User Story 5 alone. Reading,
calibration, wheel identity, the guard layer and the debug view depend on nothing missing, so the MVP and every
P1 story proceed unblocked. Two `motor` record fields are also degraded until feature 002 progresses, documented
in the protocol contract so the UI reports them as unavailable rather than as a confident negative.

### Post-Phase 1 re-check

**Status: PASS on all five principles.** Principle V was blocked pending the runtime-stack amendment; that
amendment is now applied as constitution 1.1.0, so the gate is cleared.

The design tightened two things during Phase 1. The intent token was introduced after noticing that a
confirmation enforced only in the UI would be one refactor away from being skipped. And the `abort` command was
made the sole request permitted while a request is in flight, after noticing that a strictly serialised queue
would have made the stop button wait behind the very stage it needs to interrupt.

Post-analysis remediation added three further corrections: the abort byte now has a defined handling path outside
a calibration stage as well as inside one, the false claim that the `V`, `M` and `I` commands already exist was
corrected against the firmware, and the constitution conflict was reclassified from a note to a blocking
prerequisite.

## Project Structure

### Documentation (this feature)

```text
specs/003-v13-configurator/
├── plan.md                     # This file
├── spec.md                     # 7 stories, 40 requirements, 3 clarifications resolved
├── research.md                 # Phase 0: 11 decisions, 5 hardware gates
├── data-model.md               # Phase 1: session, mirrored device state, persistence
├── quickstart.md               # Phase 1: 12 scenarios, unpowered before powered
├── contracts/
│   ├── serial-protocol.md      # The wire contract: records, tagging, abort byte
│   └── app-ipc.md              # Rust core to Vue frontend boundary
├── reference/
│   └── figma-bldc-configurator.png
├── checklists/requirements.md
└── tasks.md                    # Phase 2 output ($speckit-tasks — NOT created here)
```

### Source Code (repository root)

```text
v13_macnum_wheel_car.ino        # Firmware: command registrations, the serial abort intercept,
                                #   calibration progress calls, request tags, bus identity command
serial_records.{h,cpp}          # NEW sketch-local module: record framing and emission, keeping
                                #   the 1469-line sketch from growing further and matching the
                                #   pattern set by foc_timing / impedance_control / as5147_fast
can_protocol.h                  # Unchanged
foc_timing.{h,cpp}              # Unchanged
impedance_control.{h,cpp}       # Unchanged
as5147_fast.{h,cpp}             # Unchanged
src/MPU6050/, src/I2Cdev/       # Vendored, MUST NOT be modified

v13-configurator/               # NEW standalone desktop application
├── src/                        # Vue 3 + TypeScript frontend
│   ├── views/                  # Config and Debug, matching the reference sidebar
│   ├── components/
│   └── stores/                 # Pinia; mirrors device state, holds no parsed text
├── src-tauri/
│   ├── src/
│   │   ├── protocol/           # Record parser and encoder, pure, unit tested
│   │   ├── session/            # Port ownership, tagging, timeouts, state machine
│   │   └── storage/            # App settings and the append-only change log
│   └── capabilities/           # Serial plugin permissions
└── package.json

jetson_xavier/                  # Untouched by this feature
```

**Structure Decision**: The configurator is a sibling directory at the repository root rather than a subtree of
`jetson_xavier/`, because it deliberately does not depend on the Jetson, its network, or the vehicle bus, and
placing it under that tree would imply otherwise. Inside `src-tauri`, the protocol, session and storage concerns
are separated so the parser and the state machines stay pure and testable, with the serial plugin touched from
one place. The frontend mirrors the reference design's two-section sidebar directly.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| A second machine-readable output format on the firmware console, in addition to the existing prose | The console must serve a human bench operator and a GUI at once. Prose cannot safely be parsed by a safety-relevant tool: values are split across lines, refusals are free text, and spacing varies, so an ordinary firmware reword would silently corrupt a displayed motor parameter. | Parsing the existing prose was rejected because it creates an undocumented shadow contract over strings nobody is obliged to keep stable, which is the exact silent-drift failure the constitution's protocol principle exists to prevent. Replacing the prose was rejected because the console remains the primary bench tool for features 001 and 002. |
| A fourth runtime component, outside the constitution's enumerated stack | The clarified decision is a standalone desktop app connecting by direct serial, which cannot be delivered inside the existing three components. | Extending the Jetson-hosted UI was rejected during clarification, and would have reversed the deliberate decision that calibration and tuning live on the direct console. A browser page using Web Serial was rejected for Chromium-only support and its inability to hold a port across a reload. |
| Modifying safety-critical calibration code to add a serial abort check | Without it there is no software stop for an energised stage on a bench with no CAN bus, so the spec's always-reachable stop requirement cannot be met. | Disclosing the limitation was rejected: shipping a stop button that cannot stop anything mid-stage is worse than shipping none. Requiring a CAN interface was rejected as contradicting the direct-serial decision. Making calibration non-blocking is the better long-term fix but is a far larger change; it is recorded as future work. |

## Phase 2 preview (not executed here)

`$speckit-tasks` should sequence so that the safety prerequisite and the parser land before any GUI can energise
a motor:

1. Firmware: structured record emitters and the `Q` query family, verified by console capture against the contract.
2. Firmware: **the serial abort byte**, verified on hardware. This gates every powered scenario.
3. Firmware: calibration progress emission, request tagging, and the bus identity command.
4. App scaffold: Tauri 2 plus Vue, serial plugin permissions, port enumeration.
5. Rust core: the parser, unit tested against both valid records and firmware prose that must be ignored.
6. Rust core: session ownership, tagging, timeouts, non-resetting connect.
7. Read-only UI: identity, calibration display, both motors, staleness and version mismatch states.
8. Debug view and telemetry.
9. Guided calibration with the intent token and the always-available stop.
10. The four writable settings, then the change log and operator profile.

Steps 2 and 5 carry the risk. Step 2 is a safety gate that must be proven on hardware before the GUI is allowed
to start a stage, and step 5 is where a silent data-corruption bug would otherwise hide.
