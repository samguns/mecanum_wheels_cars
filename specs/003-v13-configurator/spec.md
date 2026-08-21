# Feature Specification: V13 Configurator

**Feature Branch**: `003-v13-configurator`

**Created**: 2026-08-21

**Status**: Draft

**Input**: User description: "Create a UI named "v13-configurator" for easier motors calibration and debug. Take https://www.figma.com/design/qyddA8TwStquAD5TYAcYhC/pov-mgmt?node-id=20-2&p=f&t=G9PskVr2Ib1zWDq6-0 as reference design"

**Reference design**: [`reference/figma-bldc-configurator.png`](./reference/figma-bldc-configurator.png), captured from the
supplied Figma node. It shows a "V13-Driver" application with a sidebar split into MAIN (BLDC Config) and DEBUG
(Control), a "BLDC Configurator — Brushless DC Motor Diagnostic & tuning Suite" page, a MOTOR PARAMETERS card
carrying pole pairs, direction, phase resistance and phase inductance with a CALIBRATE action, footer actions for
"Read from Device" and "Write Configuration", and a signed-in operator identity in the sidebar footer.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - See what a controller currently believes (Priority: P1)

As a technician commissioning a vehicle, I open the configurator, connect to a controller, and read its stored
motor parameters and calibration state, so that I can tell at a glance whether a unit is calibrated and what
values it is using, without typing serial commands or interpreting console text.

**Why this priority**: It is the foundation for everything else and the only story that produces no motion at
all. It is also independently valuable on its own: today the only way to inspect a controller is to attach a
serial terminal and read a text dump.

**Independent Test**: Connect a calibrated controller, read its configuration, and confirm the displayed pole
pairs, direction, resistance, inductance, electrical offset and calibration state match what the controller's
own text report shows. Then connect an uncalibrated controller and confirm it is clearly marked as requiring
calibration.

**Acceptance Scenarios**:

1. **Given** a connected, calibrated controller, **When** the operator requests a read, **Then** every stored
   motor parameter and the per-motor calibration state are displayed with their units.
2. **Given** a connected but uncalibrated controller, **When** the operator requests a read, **Then** the
   configurator clearly reports that calibration is required and which motor is incomplete.
3. **Given** no controller is connected, **When** the configurator is opened, **Then** it shows a disconnected
   state and offers no action that would imply a live device.
4. **Given** a read has completed, **When** the operator views the page, **Then** the values are marked as
   device-reported rather than as pending edits.
5. **Given** a controller reports a configuration format the configurator does not recognise, **When** the read
   completes, **Then** the mismatch is reported and no values are presented as trustworthy.

---

### User Story 2 - Run a guided calibration without the serial console (Priority: P1)

As a technician, I start calibration for a selected motor from the configurator, watch its progress, and accept
or reject the measured result, so that commissioning no longer requires memorising console commands.

**Why this priority**: This is the "easier calibration" the feature exists for. Calibration is currently a
multi-step console procedure with single-letter commands, which is the main source of commissioning friction.

**Independent Test**: With the vehicle secured and wheels clear, run a full alignment and characterisation
sequence for one motor entirely from the configurator, accept the result, and confirm the controller reports the
same saved values afterwards.

**Acceptance Scenarios**:

1. **Given** a selected, disarmed motor, **When** the operator starts calibration, **Then** the configurator
   first states the safety precondition and requires an explicit confirmation before anything energises.
2. **Given** calibration is running, **When** the operator watches the page, **Then** the current step, its
   progress, and the fact that the motor is energised are all visible.
3. **Given** calibration produces a result, **When** it completes, **Then** the measured values are presented as
   a pending result that the operator must explicitly accept or reject.
4. **Given** the operator accepts a result, **When** the acceptance completes, **Then** the configurator confirms
   the controller persisted it, and a subsequent read returns the same values.
5. **Given** the operator rejects a result, **When** the rejection completes, **Then** the previously stored
   values remain in force and the motor is left de-energised.
6. **Given** calibration fails or aborts on over-current, **When** the operator views the
   page, **Then** the failure reason is shown, an immediate retry is available, and no partial values are
   presented as saved.
7. **Given** calibration is running, **When** the operator triggers the configurator's stop control, **Then**
   the procedure ends and the motor is left de-energised.

---

### User Story 3 - Know which wheel you are about to energise (Priority: P1)

As a technician, I connect to one controller and choose which of its two motors I am inspecting or calibrating,
so that I never act on the wrong wheel.

**Why this priority**: A session addresses one controller, but that controller drives two motors and calibration
data is per-motor. Energising the wrong motor of the pair is a safety problem, not just an inconvenience. The
reference design shows a single unlabelled parameter set, so this needs to be explicit.

**Independent Test**: Connect one controller, select each of its two motors in turn, read each, and confirm the
displayed values and the named wheel correspond to the physically expected wheel.

**Acceptance Scenarios**:

1. **Given** a connected controller, **When** the operator views the configurator, **Then** the connected
   controller's identity and the currently selected motor are unambiguously identified at all times.
2. **Given** a motor is selected, **When** the operator starts a powered procedure, **Then** the confirmation
   step names the selected wheel explicitly.
3. **Given** the operator changes the selected motor while a pending calibration result is unaccepted, **Then**
   the configurator warns that the pending result will be discarded before it changes selection.
4. **Given** the connected controller becomes unreachable, **When** the loss is detected, **Then** the session is
   shown as disconnected and no further actions are offered.
5. **Given** both motors of the connected controller, **When** the operator views the configurator, **Then** the
   calibration state of both is visible at once, so the operator can see which of the pair still needs work.

---

### User Story 4 - Review live debug telemetry (Priority: P2)

As an integrator diagnosing a drive, I open the debug view and watch the controller's live measured values and
fault state, so that I can tell whether a problem is electrical, mechanical, or a control-tuning issue.

**Why this priority**: The "debug" half of the request. It is valuable but strictly diagnostic, and the vehicle
is commissionable without it.

**Independent Test**: With a controller connected, open the debug view and confirm measured position, velocity,
current, bus voltage, arm state and any fault causes update continuously and agree with the controller's own
report.

**Acceptance Scenarios**:

1. **Given** a connected controller, **When** the operator opens the debug view, **Then** measured position,
   velocity, current, bus voltage, per-motor arm state and mode are displayed and refresh continuously.
2. **Given** the controller reports a fault or an effort-limit cause, **When** it occurs, **Then** the cause is
   surfaced with enough detail to distinguish current limiting, voltage limiting, protection trips, timing
   faults, and command timeouts.
3. **Given** telemetry stops arriving, **When** the gap exceeds a visible staleness threshold, **Then** the
   displayed values are marked stale rather than left looking live.
4. **Given** the operator is viewing the debug view, **When** no motor is armed, **Then** the view still
   functions and clearly shows the disarmed state.

---

### User Story 5 - Write the controller's operating settings (Priority: P2)

As a technician, I adjust the controller's operating settings, write them, and get explicit confirmation that
they were persisted, so that I can set a unit's bus identity and tuning without a serial console.

**Why this priority**: The reference design makes writing a primary action, and these settings genuinely need to
be set per unit. It ranks below calibration because measured values are read-only and an accepted calibration
already persists its own result in User Story 2.

**Scope note**: The writable set is deliberately narrow. It covers the controller's bus identity, its
current-loop bandwidth, its bus-voltage protection window, and each motor's control mode. It does **not** cover
any measured electrical or sensor quantity, which FR-022 keeps read-only.

**Independent Test**: Change each writable setting, write it, power cycle the controller, read again, and confirm
the values survived; then attempt a write with a motor armed and confirm refusal.

**Acceptance Scenarios**:

1. **Given** edited values, **When** the operator writes them, **Then** the configurator shows exactly what will
   change and requires confirmation before writing.
2. **Given** a write completes, **When** the configurator reports success, **Then** the reported success reflects
   a controller acknowledgement, not merely that the request was sent.
3. **Given** any motor on the target controller is armed, **When** a write is attempted, **Then** it is refused
   with an instruction to disarm first, and nothing is written.
4. **Given** a value outside its permitted range is entered, **When** the operator attempts to write, **Then**
   the write is refused, the valid range is shown, and the stored value is unchanged.
5. **Given** a write fails or the link drops mid-write, **When** the operator views the page, **Then** the
   configurator reports that the stored state is unknown and prompts a re-read rather than assuming success.

---

### User Story 6 - Be prevented from doing something unsafe (Priority: P2)

As an operator, I am stopped from taking an action that could produce unexpected motion, and I am told why, so
that the configurator cannot become a way around the controller's own safety rules.

**Why this priority**: The configurator adds a new path to safety-critical operations. Its refusals must mirror
the controller's, or it becomes the weakest link.

**Independent Test**: Attempt each guarded action in a state where the controller refuses it, and confirm the
configurator refuses it too, for the same stated reason, without sending a conflicting request.

**Acceptance Scenarios**:

1. **Given** the controller refuses an action, **When** the refusal arrives, **Then** the configurator shows the
   controller's reason rather than a generic failure.
2. **Given** the configurator can tell an action would be refused, **When** the operator hovers or attempts it,
   **Then** the precondition is explained before the attempt is made.
3. **Given** any powered procedure is available, **When** the operator uses the configurator, **Then** a stop
   control is reachable at all times without scrolling or navigating away.
4. **Given** the configurator is open, **When** the operator looks for a way to drive the vehicle, **Then** none
   is offered: the configurator is a commissioning and diagnostic surface only.

---

### User Story 7 - Know who is connected and what they changed (Priority: P3)

As a workshop owner, I can see which operator is using the configurator and what configuration changes were made
in a session, so that a commissioning change can be traced afterwards.

**Why this priority**: The reference design shows a signed-in operator identity, and traceability is genuinely
useful for a workshop, but no commissioning task is blocked without it.

**Independent Test**: Complete a session that changes a configuration, then confirm the session records which
operator acted, which motor was affected, and what changed.

**Acceptance Scenarios**:

1. **Given** an operator identity is established, **When** the configurator is in use, **Then** that identity is
   visible and can be ended from within the interface.
2. **Given** a configuration change or accepted calibration occurs, **When** it completes, **Then** the change is
   recorded with the affected controller, motor, values, and the acting operator.
3. **Given** no identity has been established, **When** a change is attempted, **Then** the change is either
   attributed to an explicitly anonymous session or refused, consistently and visibly.

### Edge Cases

- The link to a controller drops during calibration: the configurator reports the interruption, treats the
  result as not saved, and never shows a partial measurement as confirmed.
- Two configurator sessions target the same controller at once: the second is prevented from issuing a
  conflicting powered procedure, or is clearly told another session holds the device.
- The operator navigates away or closes the page mid-calibration: the controller's own safety behaviour governs,
  and the configurator reports the unknown outcome on return rather than assuming success.
- A controller reports values outside the ranges the configurator expects: they are displayed as reported, marked
  out of range, and never silently clamped into a plausible-looking value.
- The controller stores two axis inductance values while the reference design shows a single inductance field:
  both stored values are surfaced, because hiding one would misrepresent the device.
- Calibration is attempted after a previous failure: the previous reason remains visible and a new
  operator-confirmed attempt is available immediately.
- The controller is mid-emergency-stop: the configurator reflects that state and offers no powered procedure.
- An operator edits a field, then reads from the device: unsaved edits are either preserved as clearly marked
  pending edits or discarded after a warning, never silently overwritten.
- A motor is calibrated but its partner on the same controller is not: the configurator shows per-motor state
  rather than a single unit-level verdict.
- The vehicle is on the ground rather than on stands: the configurator cannot detect this, so the safety
  confirmation names the physical precondition explicitly instead of implying the system has verified it.

## Requirements *(mandatory)*

### Functional Requirements

#### Device connection and identity

- **FR-001**: The configurator MUST show, at all times, whether it is connected to a controller and which
  controller and motor are selected.
- **FR-002**: The configurator MUST allow the operator to choose which connected device to work with, and then to
  select either motor of that controller, identifying each motor by the wheel it drives.
- **FR-003**: The configurator MUST detect and report loss of connection, and MUST withdraw actions that require
  a live device while disconnected.
- **FR-004**: The configurator MUST report a configuration-format mismatch between what a controller provides
  and what it expects, and MUST NOT present mismatched values as trustworthy.

#### Reading configuration

- **FR-005**: The configurator MUST read and display, per motor, the pole-pair count, sensor direction, electrical
  offset, phase resistance, both axis inductance values, the derived current-loop settings, and the per-motor
  calibration completion state.
- **FR-006**: The configurator MUST display a unit for every physical quantity it shows.
- **FR-007**: The configurator MUST visually distinguish device-reported values from operator edits that have not
  been written.
- **FR-008**: The configurator MUST allow the operator to re-read on demand and MUST show when the displayed data
  was last refreshed.

#### Guided calibration

- **FR-009**: The configurator MUST let the operator run the controller's calibration procedure for a selected
  motor, covering both the alignment stage and the electrical characterisation stage.
- **FR-010**: The configurator MUST require an explicit operator confirmation, naming the selected wheel and the
  physical safety precondition, before any procedure that energises a motor.
- **FR-011**: The configurator MUST display the active calibration stage, its progress, and an indication that
  the motor is energised. The energised indication MUST be identifiable by an operator who has not been told
  where to look, within 2 seconds, without scrolling or opening another view.
- **FR-012**: The configurator MUST present a completed calibration as a pending result and MUST require an
  explicit accept or reject decision before it is treated as stored.
- **FR-013**: The configurator MUST confirm persistence of an accepted result on the basis of a controller
  acknowledgement, and MUST surface a subsequent verification read.
- **FR-014**: The configurator MUST report calibration failure and abort conditions with the controller's
  stated reason, without imposing a retry delay.
- **FR-015**: The configurator MUST offer a stop control that is reachable at all times while any powered
  procedure is possible.
- **FR-016**: The configurator MUST NOT weaken or bypass any precondition the controller enforces. Where it can
  see that a precondition is unmet it MUST explain that, and it MAY disable the action to prevent a pointless
  attempt; what it MUST NOT do is substitute its own verdict for the controller's when a request is actually
  issued, or suppress a refusal the controller returns.

#### Writing configuration

- **FR-017**: The configurator MUST let the operator write the controller's writable operating settings, showing a
  comparison of current and proposed values before the write. The writable set is the controller's bus identity,
  its current-loop bandwidth, its bus-voltage protection window, and each motor's control mode; it excludes every
  measured quantity covered by FR-022.
- **FR-018**: The configurator MUST require confirmation before writing, and MUST report success only on a
  controller acknowledgement.
- **FR-019**: The configurator MUST validate every entered value against its permitted range before writing, and
  MUST refuse an out-of-range write while reporting the valid range.
- **FR-020**: While any motor on the target controller is armed, the configurator MUST prevent a write from being
  submitted and MUST say that a disarm is required. If a write nevertheless reaches the controller and is refused,
  the controller's own refusal MUST still be surfaced rather than replaced by the local message, consistent with
  FR-016.
- **FR-021**: The configurator MUST report an interrupted or failed write as leaving the stored state unknown, and
  MUST prompt a re-read rather than displaying the attempted values as stored.
- **FR-022**: The configurator MUST present every measured electrical and sensor quantity as read-only. Pole-pair
  count, sensor direction, electrical offset, phase resistance and both axis inductance values MUST NOT be
  manually editable, and the only way to change them MUST be to run calibration and accept its measured result.
- **FR-022a**: Where the reference design shows these quantities as editable input fields, the configurator MUST
  render them as display fields instead, so no control implies an unverified value can be typed in.
- **FR-022b**: The configurator MUST make clear, next to the read-only values, that changing them requires
  running calibration, so the operator is not left hunting for a disabled edit control.

#### Debug and telemetry

- **FR-023**: The configurator MUST provide a debug view showing, per motor, the measured position, velocity and
  current, plus bus voltage, arm state and active control mode.
- **FR-024**: The configurator MUST refresh telemetry continuously while the view is open and MUST mark values
  stale when updates stop.
- **FR-025**: The configurator MUST surface controller fault and effort-limit causes distinguishably, covering at
  least current limiting, voltage limiting, protection trips, timing faults, and command timeouts.
- **FR-026**: The configurator MUST display the controller's control-timing state, including its configured and
  active current-loop bandwidth and whether the active value was reduced from the requested one.
- **FR-027**: The configurator MUST make clear which displayed quantities are controller-reported measurements
  and which it derived itself. The derived set is exactly: the wheel label, the control-loop duty percentage, the
  data-freshness age, and any out-of-range flag. Everything else displayed MUST be a value the controller
  reported.

#### Scope and safety boundaries

- **FR-028**: The configurator MUST NOT offer any vehicle driving or motion-commanding control; it is a
  commissioning and diagnostic surface only.
- **FR-029**: The configurator MUST show each motor's arm state on the same view as any powered action it offers,
  visible without scrolling and without opening another view.
- **FR-030**: The configurator MUST surface controller refusals verbatim in meaning, naming the affected motor,
  rather than reducing them to a generic error.
- **FR-031**: The configurator MUST leave every motor de-energised when a procedure ends, whether it succeeded,
  failed, or was stopped.

#### Connectivity model

- **FR-032**: The configurator MUST connect directly to one controller's serial port from the technician's own
  machine, keeping commissioning on the same direct link the controller already treats as the only authority for
  calibration and tuning.
- **FR-032a**: The configurator MUST work against a single controller connected on a bench, and MUST NOT depend
  on the vehicle's onboard computer, its network, or the vehicle bus being present or powered. This MUST be
  verified in a configuration where all three are absent.
- **FR-032b**: The configurator MUST let the operator choose which serial port to use, report the connection
  attempt's outcome, and recover from a port that disappears while in use.
- **FR-032c**: The configurator MUST confirm that the device on the chosen port is a compatible controller
  before offering any action, and MUST refuse to act on an unrecognised device.
- **FR-033**: *Merged into FR-032a.* This identifier is retired rather than reused, so existing references stay
  unambiguous. It duplicated FR-032a's bench-independence requirement.

#### Multi-motor scope

- **FR-034**: The configurator MUST address exactly one connected controller per session, and MUST allow the
  operator to select either of that controller's two motors within the session.
- **FR-034a**: The configurator MUST show the per-motor calibration state of both motors on the connected
  controller at once, so an operator can see which of the pair still needs work without switching selection.
- **FR-034b**: The configurator MUST scope every powered procedure to the single selected motor, leaving the
  other motor of the pair de-energised.
- **FR-034c**: Commissioning a full four-wheel vehicle is a sequence of connections, one controller at a time.
  The configurator MUST make the identity of the connected controller unambiguous so an operator can tell which
  pair of wheels they have already done.

#### Session identity

- **FR-035**: The configurator MUST display the current operator identity when one is established, and MUST allow
  ending that session from within the interface.
- **FR-036**: The configurator MUST record every accepted calibration and configuration write with the affected
  controller, motor, resulting values, and the acting operator identity.

### Terminology

One concept, one name. "**Controller**" is used throughout this specification. The CAN protocol contract calls
the same thing a "node" and identifies it by `NodeID`, because that is the established wire vocabulary from
features 001 and 002. The two terms are interchangeable: controller `0x201` is node `0x01`.

### Key Entities

- **Controller**: A reachable motor-control unit, identified by its configured bus identity, holding
  configuration for the two motors it drives and reporting its own live state. Called a node in the wire
  protocol.
- **Motor configuration record**: The per-motor stored values the configurator reads and may write: pole-pair
  count, sensor direction, electrical offset, phase resistance, both axis inductance values, and the completion
  state of each calibration stage.
- **Calibration session**: A time-bounded, operator-attended procedure against one selected motor, with a stage,
  progress, an energised indication, and an outcome that is pending until explicitly accepted or rejected.
- **Pending result**: A measured calibration outcome that is displayed but not yet stored, and is discarded by
  rejection, by changing selection, or by an interruption.
- **Telemetry snapshot**: The continuously refreshed set of controller-reported measurements and fault causes,
  carrying its own freshness so staleness is visible.
- **Change record**: An entry describing one accepted calibration or configuration write, with the target,
  the values, the time, and the acting operator.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A technician who has never used the serial console completes a full calibration of one motor using
  only the configurator, with no console access, on the first attempt.
- **SC-002**: Calibrating both motors of one controller takes at most half the wall-clock time the console
  procedure takes for the same work, measured across three operators.
- **SC-003**: 100% of configurator refusals state a reason and the precondition that would satisfy them.
- **SC-004**: 100% of accepted calibrations and configuration writes are confirmed by a verification read showing
  the same values, and survive a power cycle.
- **SC-005**: In 20 deliberately interrupted procedures, covering link loss, page closure, and operator stop,
  no interrupted result is ever displayed as stored, and every motor ends de-energised.
- **SC-006**: In 20 attempts to act on a controller that refuses the action, the configurator's stated reason
  matches the controller's own reported reason every time.
- **SC-007**: An operator identifies which physical wheel is selected, without ambiguity, in 100% of a 10-trial
  observation covering both motors on the connected controller.
- **SC-008**: Telemetry in the debug view is marked stale within 2 seconds of updates stopping, in 20 trials.
- **SC-009**: An operator diagnosing an induced fault, such as an over-current abort or a command timeout,
  identifies the cause from the debug view alone in under 1 minute in 4 of 5 trials.
- **SC-010**: The configurator offers zero controls capable of commanding vehicle motion, confirmed by
  inspection of every screen.
- **SC-011**: A replacement unit is brought to a fully calibrated, correctly identified state using only the
  configurator, in under 20 minutes for both of its motors, with no measured value having been typed in by hand.
- **SC-011a**: Each writable operating setting can be changed, written, and confirmed by a verification read in
  under 1 minute.
- **SC-012**: Every accepted calibration and write in a 10-change session is retrievable afterwards with its
  target, values and acting operator.
- **SC-013**: In 5 trials with an operator who has not been told where to look, the energised indication and the
  selected motor's arm state are both identified within 2 seconds, without scrolling or changing view.
- **SC-014**: The configurator completes a full read, a calibration, and a setting write with the vehicle's
  onboard computer powered off and no vehicle bus connected.

## Assumptions

- The controller's existing safety model is authoritative and unchanged. The configurator is a new operator
  surface over it, not a new set of rules; where the two could disagree, the controller wins.
- Calibration remains an operator-attended bench procedure with the vehicle secured and the wheels clear of the
  ground. The configurator cannot verify that physically, so it states the precondition and requires explicit
  confirmation rather than implying it has been checked.
- The reference design is treated as the intended information architecture and interaction model: a sidebar
  separating configuration from debug, a motor-parameters card with a calibrate action, and explicit read and
  write actions rather than continuous silent syncing. Exact visual styling is a design-time concern and is not
  specified here.
- The reference design shows a single phase-inductance field, while the controller stores a separate value per
  electrical axis. Both stored values are surfaced, because showing one would misrepresent the device.
- The reference design shows no electrical-offset field, although calibration produces one. It is treated as a
  read-only reported value so the operator can verify a calibration rather than edit it blindly.
- A replacement unit is commissioned by calibrating it, not by copying another unit's measured values. Copying
  measured electrical values between physical motors was considered and excluded, because the values describe one
  specific motor and its sensor installation.
- Because a session addresses one controller, commissioning a four-wheel vehicle is a sequence of two connections.
  The configurator is not expected to coordinate them or track cross-controller progress beyond making the
  connected controller's identity unambiguous.
- The reference design shows a signed-in operator identity, so a notion of session identity is assumed to be in
  scope at least for display and attribution. Choosing an identity provider, and whether access is restricted
  rather than merely attributed, is deferred to planning.
- Live telemetry is expected to be human-readable at a glance rather than instrument-grade: the configurator
  reports what the controller already measures and does not add its own signal processing or high-rate capture.
- The existing driving interface remains the surface for operating the vehicle. This feature does not remove or
  change it, and the two are not required to be used at the same time.
- No change to the controller's stored configuration format is assumed. If the configurator needs a value the
  controller does not currently expose, exposing it is a controller-side change to be identified during planning.
- The vehicle's two controllers may be at different calibration states, and a bench unit may be entirely
  uncalibrated. Both are normal starting conditions rather than error cases.
