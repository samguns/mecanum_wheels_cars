<!--
Sync Impact Report
- Version change: template -> 1.0.0
- Modified principles: template placeholders replaced with 5 project-specific principles
- Added sections: Additional Constraints, Development Workflow
- Removed sections: none
- Follow-up TODOs: set ratification date on adoption

Amendment 1.1.0 (2026-08-21)
- Version change: 1.0.0 -> 1.1.0 (MINOR: adds a supported component and new rules governing it)
- Modified sections: Additional Constraints > Runtime and Deployment Constraints (adds the desktop
  configurator and the constraints binding it); Development Workflow > Change Gates (adds the
  validation gate for a desktop-app change)
- Modified principles: none. All five core principles are unchanged.
- Removed sections: none
- Why the current rule was insufficient: the Runtime and Deployment Constraints enumerated four
  supported components and required all changes to preserve that stack. Feature 003 (the V13
  Configurator) is a standalone desktop application and therefore a fifth component, so it could not
  proceed without either an amendment or a reinterpretation of a normative MUST. Reinterpretation is
  not permitted by this document's own Governance section, so the enumeration is amended explicitly.
- Migration impact:
  - Firmware: none to existing behaviour. Feature 003 adds a structured serial response format
    alongside the existing human console output, and a serial abort path. No existing command changes.
  - Python backend: none. The configurator deliberately does not depend on it.
  - Vue operator UI: none. It remains the only surface that commands vehicle motion.
  - Deployment: adds a desktop application built and distributed separately from the Docker images.
    Nothing about the existing Docker workflow changes.
- Follow-up TODOs: still pending from 1.0.0, set the ratification date on adoption.

Amendment 1.2.0 (2026-08-22)
- Version change: 1.1.0 -> 1.2.0 (MINOR: adds a new core principle)
- Modified principles: none renamed. Added VI. Rust Must Be Written as Rust.
- Modified sections: Development Workflow > Change Gates (Rust-native validation);
  Development Workflow > Review Expectations (ownership, invalid states, panic paths);
  Governance (compliance review now includes first-party Rust)
- Removed sections: none
- Why the current rule was insufficient: Principles III and V and the desktop-app change
  gate require a native test run for parser and encoder logic, but they do not bind first-party
  Rust to the language's own invariants. After 1.1.0 the configurator is a supported runtime
  component. Without an explicit rule, Rust is free to take C++- or TypeScript-shaped form
  (stringly protocol state, unwrap on I/O, clone-as-escape, interior mutability as default,
  god-object sessions). That is a governance gap, not a style preference: those shapes hide
  the same class of silent misreport the parser tests exist to catch.
- Migration impact:
  - Firmware: none.
  - Python backend: none.
  - Vue operator UI: none.
  - Deployment: none to existing Docker workflow.
  - Configurator / any first-party Rust: new code MUST satisfy Principle VI from this
    amendment forward. Existing first-party Rust that predates 1.2.0 MUST be brought into
    compliance before the next behaviour-changing change to that crate, or in the same
    change that touches it.
- Follow-up TODOs: still pending from 1.0.0, set the ratification date on adoption;
  review `v13-configurator/src-tauri` against Principle VI on the next crate change.
-->

# Mecanum Wheel Car Constitution

## Core Principles

### I. Safety-Critical Motion First
Any change that can affect motor direction, wheel mixing, enable flags,
emergency stop behavior, current limits, or CAN command encoding MUST be treated
as safety-critical. Such changes MUST preserve a deterministic zero-output path,
MUST define the expected behavior for forward, reverse, strafe, and rotation,
and MUST be validated on both a bench-safe path and a hardware path before
release.

Rationale: This repository drives physical actuators. A sign error or stale
enable path is a behavioral defect, not a cosmetic bug.

### II. Protocol Compatibility Is a Hard Contract
The CAN message schema between the backend and ESP32 firmware MUST remain
explicit, versioned in practice, and backward-compatible unless the firmware and
backend are updated together. Message IDs, byte layout, scaling, signedness,
left/right motor mapping, and estop semantics MUST be documented in the same
change that modifies them.

Rationale: The backend in `jetson_xavier/backend/socketio_server.py` and the
firmware in `v13_macnum_wheel_car.ino` form a tightly coupled protocol boundary
where silent drift will produce dangerous motion bugs.

### III. Validate at the Boundary You Changed
Every change MUST be validated at the narrowest executable boundary that can
falsify it. Mixer and protocol changes require function-level or message-shape
checks first; backend API changes require a focused Python run or request-level
check; frontend control changes require a local build or type-check; firmware
changes require compile validation and, when motion-related, an on-hardware
verification note.

Rationale: This codebase spans firmware, backend, and UI. Broad end-to-end
testing alone is too slow and too ambiguous for reliable debugging.

### IV. Configuration Must Not Be Hard-Coded into Behavior
Network addresses, Wi-Fi credentials, CAN interface assumptions, geometry
values, and similar deployment-specific values MUST live in configuration
surfaces, not fixed literals in control logic. New hard-coded secrets or
environment-specific endpoints are prohibited. Existing literals should be
removed opportunistically when touching the surrounding code.

Rationale: The current codebase contains device-local settings and addresses
that make reuse, review, and deployment harder and less safe.

### V. Preserve Clear Ownership Boundaries
Changes MUST stay within the owning layer unless the behavior genuinely crosses
boundaries. Firmware owns actuator execution and sensor/motor setup. The Python
backend owns joystick-to-CAN translation, transport, and runtime control APIs.
The Vue UI owns operator interaction and display state. Vendor code and imported
libraries under `src/MPU6050` and `src/I2Cdev` MUST NOT be modified unless the
change is unavoidable and explicitly justified.

Rationale: Most regressions in mixed hardware/software repos come from boundary
confusion and untracked edits to vendored code.

### VI. Rust Must Be Written as Rust
First-party Rust in this repository MUST conform to Rust's own ownership,
type-system, and error-handling model. This is a correctness rule, not a style
preference.

- Ownership and borrowing MUST be modelled explicitly. `Rc`/`Arc`, `Mutex`,
	`RefCell`, and other interior mutability MUST be used only when shared
	mutation is required, and that requirement MUST be visible at the type.
- Invalid combinations MUST be unrepresentable. Protocol records, session
	phases, and command outcomes MUST be enums or newtypes, not stringly or
	boolean bags that permit illegal pairs.
- Fallible work MUST return `Result` or `Option`. Production paths MUST NOT
	`unwrap`, `expect`, or `panic` except for a broken programmer invariant.
	I/O, parsing, and device protocol paths are never such an invariant.
- `unsafe` MUST be justified in place (what is trusted, why safe Rust cannot
	express it, and what keeps it sound). Undocumented `unsafe` is prohibited.
- Shared behaviour MUST be expressed with traits and modules, not
	inheritance-shaped structs or god objects that own every concern.
- Public API surface MUST stay small. Fields stay private unless they are the
	published contract.
- `clone`, `to_string`, and collect-to-own MUST NOT be used as a borrow-checker
	escape. Each owned copy MUST have a stated reason (store, send, or outlive).
- Tests that constrain a module MUST live with that module (`#[cfg(test)]` in
	the same file, or an adjacent unit test the module owns).

Rationale: Rust's value in this repo is making silent misreports and aliasing
bugs unrepresentable. Code that compiles while ignoring those tools is C-shaped
Rust and MUST be rejected in review.

## Additional Constraints

### Runtime and Deployment Constraints
The supported runtime stack is:

- ESP32 Arduino firmware at the repository root for motor control and CAN
	handling.
- Python backend under `jetson_xavier/backend` for Socket.IO, FastAPI, and CAN
	bus publishing.
- Vue 3 + Vite operator UI under `jetson_xavier/webUI`.
- Docker support under `jetson_xavier/docker` for reproducible backend and UI
	environments.
- Desktop commissioning and diagnostic application under `v13-configurator`,
	connecting directly to one controller's serial port.

All changes MUST preserve these boundaries unless the work explicitly includes
an architecture migration.

Adding a further component to this list requires an amendment, not a
reinterpretation of this section.

### Commissioning Tool Constraints
A commissioning or diagnostic tool that connects directly to a controller MUST
observe the following. These constraints exist because such a tool reaches the
actuators over a path that bypasses the backend entirely.

- It MUST NOT offer any control capable of commanding vehicle motion. Driving
	remains exclusively the operator UI's responsibility.
- It MUST NOT implement, duplicate, or override any safety rule the firmware
	enforces. Where it can see a precondition is unmet it MAY explain that and
	disable the action, but the firmware remains the sole authority on refusal.
- It MUST provide a stop path that is effective at every moment a motor can be
	energised, and that stop path MUST be verified on hardware before release.
- Any machine-readable interface it relies on MUST be explicitly versioned and
	documented, and MUST change together with the firmware that serves it. A tool
	MUST NOT depend on parsing output intended for human readers.
- It MUST NOT be required for normal vehicle operation. The vehicle MUST remain
	fully operable with the tool absent.

Rationale: a bench tool that can energise a motor is a safety-critical surface,
but it is not on the vehicle's normal control path, so the guarantees it must
provide have to be stated rather than inherited from the backend and UI rules.

### Observability and Debugging
Safety-relevant paths MUST emit enough structured diagnostics to reconstruct
command flow, controller state, and failure causes. Debug output is allowed
during development, but noisy logging in hot paths SHOULD be gated, reducible,
or removable before release. Operator-visible failures MUST fail closed where
possible.

### Vendor and Generated Content
The contents of `src/MPU6050`, `src/I2Cdev`, and generated dependency trees such
as `node_modules` SHOULD be treated as external dependencies. Repository changes
SHOULD target first-party files before patching vendored code. If vendored code
must change, the reason, source version, and local delta MUST be documented.

## Development Workflow

### Change Gates
Every motion-affecting change MUST include:

- A short statement of the expected wheel or actuator behavior.
- One focused validation artifact, such as a mixer calculation check, CAN
	payload check, or firmware compile result.
- A note describing whether hardware verification was completed, deferred, or
	blocked.

Every frontend or backend change MUST include the narrowest available build,
type-check, or runtime validation for the touched surface.

Every desktop commissioning tool change MUST include the narrowest available
validation for the touched surface: a native test run for logic that parses or
encodes a device interface, and a type-check or build for the interface layer. A
change to the machine-readable interface between the tool and the firmware MUST
be validated on both sides in the same change.

Every first-party Rust change MUST additionally compile and pass `cargo test`
for the touched crate, and MUST NOT introduce a Principle VI violation
(`unwrap`/`expect` on I/O or protocol paths, undocumented `unsafe`, or a newly
representable illegal state). Existing first-party Rust that predates
constitution 1.2.0 MUST be brought into compliance in the same change that
touches it, or before the next behaviour-changing change to that crate.

### Review Expectations
Code review MUST prioritize:

- Motion safety and estop correctness.
- Left/right and front/rear sign conventions.
- CAN payload compatibility.
- Hard-coded environment or secret values.
- Unnecessary edits to vendored or generated content.
- For first-party Rust: ownership honesty, unrepresentable illegal states,
	`Result` on fallible paths, and absence of undocumented `unsafe`.

Review summaries that only discuss style are insufficient for control-path
changes.

### Spec Kit Usage in This Repository
Spec Kit SHOULD be used for behavior-changing work that spans more than one
subsystem or changes runtime contracts. For small local fixes, direct
implementation is acceptable, but any change that alters operator controls, CAN
protocol behavior, firmware control semantics, deployment assumptions, or safety
behavior SHOULD start with a Spec Kit artifact flow:

- `/speckit.constitution` for governance changes.
- `/speckit.specify` for behavior changes.
- `/speckit.plan` for cross-layer implementation design.
- `/speckit.tasks` before execution of multi-file work.

## Governance

This constitution governs firmware, backend, UI, first-party Rust, and
deployment work in this repository. When a task conflicts with this document,
the constitution wins unless it is explicitly amended.

Amendments MUST:

- Explain why the current rule is insufficient.
- Identify any migration impact on firmware, backend, UI, or deployment
	workflows.
- Bump the constitution version using semantic versioning.

Versioning policy:

- MAJOR: Removes or materially weakens a safety, protocol, or validation
	requirement.
- MINOR: Adds a new principle, section, or materially stronger rule.
- PATCH: Clarifies wording without changing governance meaning.

Compliance review is mandatory for pull requests or direct changes affecting
motion control, CAN protocol behavior, estop handling, first-party Rust, or
deployment/runtime configuration.

**Version**: 1.2.0 | **Ratified**: TODO(RATIFICATION_DATE): set on adoption |
**Last Amended**: 2026-08-22
