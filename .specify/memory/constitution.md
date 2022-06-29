<!--
Sync Impact Report
- Version change: template -> 1.0.0
- Modified principles: template placeholders replaced with 5 project-specific principles
- Added sections: Additional Constraints, Development Workflow
- Removed sections: none
- Follow-up TODOs: set ratification date on adoption
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

All changes MUST preserve these boundaries unless the work explicitly includes
an architecture migration.

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

### Review Expectations
Code review MUST prioritize:

- Motion safety and estop correctness.
- Left/right and front/rear sign conventions.
- CAN payload compatibility.
- Hard-coded environment or secret values.
- Unnecessary edits to vendored or generated content.

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

This constitution governs firmware, backend, UI, and deployment work in this
repository. When a task conflicts with this document, the constitution wins
unless it is explicitly amended.

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
motion control, CAN protocol behavior, estop handling, or deployment/runtime
configuration.

**Version**: 1.0.0 | **Ratified**: TODO(RATIFICATION_DATE): set on adoption |
**Last Amended**: 2026-08-10
