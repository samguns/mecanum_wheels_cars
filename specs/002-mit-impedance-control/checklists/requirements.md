# Specification Quality Checklist: MIT Impedance Control Mode with Deterministic Current Loop

**Purpose**: Validate specification completeness and quality before proceeding to planning
**Created**: 2026-08-20
**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No implementation details (languages, frameworks, APIs)
- [x] Focused on user value and business needs
- [x] Written for non-technical stakeholders
- [x] All mandatory sections completed

## Requirement Completeness

- [x] No [NEEDS CLARIFICATION] markers remain
- [x] Requirements are testable and unambiguous
- [x] Success criteria are measurable
- [x] Success criteria are technology-agnostic (no implementation details)
- [x] All acceptance scenarios are defined
- [x] Edge cases are identified
- [x] Scope is clearly bounded
- [x] Dependencies and assumptions identified

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria
- [x] User scenarios cover primary flows
- [x] Feature meets measurable outcomes defined in Success Criteria
- [x] No implementation details leak into specification

## Notes

- Validation iteration 1: three [NEEDS CLARIFICATION] markers were raised, all scope- or safety-impacting.
- Validation iteration 2: all three resolved by operator decision and folded into the spec.
  - FR-006 — the legacy velocity command is retained **permanently** as a supported second mode alongside impedance. Added FR-006a through FR-006c (per-motor active mode, zeroing on mode change, shared safety rules) and User Story 4.
  - FR-007 originally selected one impedance frame per motor. The later 2026-08-20 remediation clarification supersedes that choice with an atomic two-frame pair so absolute accumulated position remains representable.
  - FR-020 — the **switching carrier is raised** where hardware allows so higher bandwidths stay reachable. Added FR-020a through FR-020c: current-sensing validity must be verified, and the carrier never changes while armed. (Superseded in the clarify session below: an unreachable bandwidth is clamped, not rejected.)
- Consistency fix during iteration 2: FR-006a originally required an explicit mode selection before motion, which contradicted SC-010 (unmodified velocity sender keeps working). Velocity mode is now the power-up default, with startup safety resting on both motors being disarmed.
- Clarify session 2026-08-20: five clarifications recorded in the spec's Clarifications section and integrated.
  - Unreachable bandwidth is now **clamped** to what the maximum permissible carrier supports, never rejected for that reason (FR-020b). The requested value is persisted and the active value re-derived at startup (FR-020d).
  - Position targets are **absolute accumulated shaft angle** with a saturated position-error term (FR-001a through FR-001c), which removes the runaway-torque hazard on a continuously rotating wheel.
  - Scope is **all three layers**: firmware, backend sender, and operator UI (FR-036 through FR-042, User Story 7).
  - Command contract fixed at **200 Hz commands with a 50 ms timeout** (FR-029, FR-038).
  - Bandwidth is **serial-write only**, reported over the bus read-only (FR-021a through FR-021c, FR-040a).
- Re-validation after clarify: 16/16 items still passing, no state changes. Fixed requirement/criterion ordering and aligned "configured" versus "active" versus "stored" bandwidth wording.
- Remediation clarification 2026-08-20: replaced the contradictory ±12.5 rad absolute-position field with a sequence-matched two-frame command carrying signed 32-bit milliradians; added measured-position CAN readback, atomic-pair failure handling, and explicit current/output-voltage/bus-voltage limit reporting and validation. Re-validation remains 16/16 with no checkbox state changes.
- Follow-up clarification 2026-08-20: fixed the sampling multiple at 10, separated normal-stop/estop/link-failure transmission, chose persisted per-motor modes with disarmed zero-state startup, added controller-local stiffness target capture with acknowledgement, and selected exhaustive-software plus fixed-matrix hardware bandwidth validation. Re-validation remains 16/16 with no checkbox state changes.
- All checklist items pass. Spec is ready for `$speckit-plan`.
- Open items deliberately deferred to measurement, not blocking: the exact maximum permissible switching carrier and highest bandwidth it yields, final protection thresholds and dynamics ranges, and confirmation of the estimated ~27% worst-case bus utilization for eight motion frames per update.
