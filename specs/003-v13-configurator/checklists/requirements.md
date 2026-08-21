# Specification Quality Checklist: V13 Configurator

**Purpose**: Validate specification completeness and quality before proceeding to planning
**Created**: 2026-08-21
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

- Validation iteration 1: three [NEEDS CLARIFICATION] markers raised, all scope- or safety-impacting.
- Validation iteration 2: all three resolved by operator decision and folded into the spec.
  - **FR-032** — the configurator connects **directly to one controller's serial port** from the technician's
    machine, keeping commissioning on the link the controller already treats as the sole authority for
    calibration and tuning. Added FR-032a through FR-032c: no dependency on the onboard computer, network or
    vehicle bus; operator-chosen port with recovery from a vanishing port; and device-compatibility confirmation
    before any action is offered.
  - **FR-034** — **one controller per session, either of its two motors** selectable. Added FR-034a through
    FR-034c: both motors' calibration state visible at once, powered procedures scoped to the selected motor
    only, and unambiguous controller identity so a four-wheel vehicle can be done as two sequential connections.
  - **FR-022** — measured quantities are **read-only**; the only way to change them is to run calibration and
    accept the result. Added FR-022a and FR-022b: the reference design's editable fields become display fields,
    and the interface says that changing them requires calibration so the operator is not hunting for a disabled
    control.
- Consistency fixes during iteration 2, all caused by the read-only decision cascading into the write story:
  - User Story 5 was "write a reviewed configuration"; with every measured value read-only that was hollow. It is
    now scoped to the genuinely writable operating settings (bus identity, current-loop bandwidth, bus-voltage
    protection window, per-motor control mode), and FR-017 states that set explicitly.
  - SC-011 assumed a configuration could be transferred onto a replacement unit. Copying measured values between
    physical motors was explicitly rejected, so it now measures commissioning a replacement unit by calibration,
    with a new SC-011a for writing an operating setting.
  - FR-002 and User Story 1 still spoke of selecting among multiple reachable controllers, contradicting the
    one-controller-per-session decision; both were corrected.
  - User Story 3 was retitled and rescoped from choosing among controllers to identifying which wheel of the
    connected pair is about to be energised.
- Design-versus-device mismatches found while reading the reference and recorded as assumptions rather than
  clarifications, because a reasonable default exists in each case: the single inductance field versus two stored
  axis values, the absent electrical-offset field, and the absent motor selector.
- All checklist items pass. Spec is ready for `$speckit-plan`.
- Post-analysis remediation, 2026-08-21, after `$speckit-analyze` found 14 issues including 2 CRITICAL:
  - **FR-033 retired**, its content merged into FR-032a, which duplicated it. The identifier is left in place as
    a retirement marker rather than reused, so existing references stay unambiguous.
  - **FR-011 and FR-029 made measurable.** "Unmistakable" and "prominently" carried no criterion; both now
    specify identification within 2 seconds without scrolling, verified by new SC-013.
  - **FR-016 and FR-020 reconciled.** They previously disagreed on whether the app may pre-empt a controller
    precondition. FR-016 now permits disabling an action while forbidding substituting a local verdict for the
    controller's; FR-020 defers to it explicitly.
  - **FR-027 made concrete** by naming the four configurator-derived quantities, so "which are calculations" is
    now testable.
  - **FR-032a strengthened** to require verification with the Jetson and vehicle bus absent, added as SC-014.
  - **Terminology section added**, recording that "controller" and the wire protocol's "node" are the same thing.
- Requirement count unchanged at 44 (FR-033 retired in place); success criteria grew from 13 to 15.
- Deferred to planning, not blocking: the identity provider and whether access is restricted rather than merely
  attributed; the telemetry refresh rate and staleness threshold; whether any controller-side change is needed to
  expose a value the configurator wants; and the browser serial-access mechanism implied by the direct-connection
  decision.
