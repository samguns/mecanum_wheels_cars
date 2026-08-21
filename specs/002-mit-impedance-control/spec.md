# Feature Specification: MIT Impedance Control Mode with Deterministic Current Loop

**Feature Branch**: `002-mit-impedance-control`

**Created**: 2026-08-20

**Status**: Draft

**Input**: User description: "Current implementation is a velocity controller, let's migrate to the MIT impedance mode. The current-loop bandwidth is a user configurable parameter in the range of 100 - 10000 Hz. The default value is 1000Hz. Given this parameter, make the sampling and control loop deterministic"

## Clarifications

### Session 2026-08-20

- Q: What validation scope must cover bandwidth derivation, persistence, clamping, and current-sense accuracy? → A: Exhaustively test the pure derivation and validation helpers for every integer request from 100 through 10000 Hz plus malformed and out-of-range inputs. On hardware, use the fixed matrix 100, 500, 1000, measured sustainable ceiling, ceiling plus one, and 10000 Hz for acceptance, clamp reporting, and power-cycle persistence; verify current accuracy at the lowest, default-derived, and highest active carrier points.
- Q: How must a hold target be established safely when an armed motor transitions from zero to non-zero stiffness? → A: The matched dynamics half carries a capture-current-position flag. In the same control cycle that applies a flagged pair, the controller replaces the transmitted position target with its measured accumulated position and reports the applied target. The sender keeps requesting capture without assuming a target until it receives that controller confirmation, then sends the confirmed absolute target on later pairs.
- Q: Should each motor's motion mode persist across power cycles? → A: Persist and restore the last accepted per-motor mode, while always starting disarmed with all targets, gains, and effort zeroed. A fresh or invalid configuration defaults to velocity; backward compatibility with an unmodified velocity sender requires the stored mode to be velocity.
- Q: How must the sender handle motion frames during normal idle or stop, emergency stop, and a communications failure? → A: During normal idle or stop, continue sending explicit zero-effort motion commands at 200 Hz. On emergency stop, immediately send the existing 0x080 emergency-stop frame and suspend motion frames while it remains engaged. Only a link or sender failure goes silent and relies on the 50 ms controller timeout.
- Q: What should the controller do if hardware measurements cannot sustain the default 1000 Hz bandwidth at the required sampling multiple? → A: Keep the sampling rate at least ten times the active bandwidth; never reduce the sampling multiple to reach 1000 Hz. Preserve 1000 Hz as the default requested value, clamp the active bandwidth to the measured sustainable value, and report the clamp.
- Q: How must accumulated position targets be encoded so continuous rotation and the 100-revolution safety test remain representable? → A: Use an atomic two-frame command per motor: a signed 32-bit absolute accumulated-position target in milliradians plus a paired dynamics frame carrying the other impedance terms; both frames carry the same sequence number and are applied only as a matched pair.
- Q: When a requested bandwidth needs a switching carrier above what the hardware allows, should it be rejected or clamped? → A: Clamp to the highest bandwidth the maximum permissible carrier supports, and report the clamp; never reject a value that is inside the 100–10000 Hz range for this reason.
- Q: On a continuously rotating wheel, what does the impedance position target refer to? → A: The absolute accumulated shaft angle since power-up, following the MIT convention, with the position-error term saturated at a documented maximum so stiffness cannot request runaway torque.
- Q: Which layers does this feature cover — firmware only, firmware and backend, or all three? → A: All three: firmware, the Jetson backend sender, and the operator UI, including new operator controls for stiffness, damping, and mode selection.
- Q: What motion command rate and command timeout should the contract specify? → A: A 200 Hz nominal command rate per motor with a 50 ms command timeout, so a controller tolerates nine consecutive lost frames before it zeroes effort.
- Q: Which channels carry bandwidth configuration, mode selection, and status readback? → A: Mode selection and status readback travel over CAN; bandwidth is writable only from the serial console, with its active value reported over CAN for display.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Command a wheel with an impedance law (Priority: P1)

As the vehicle controller, I send each drive motor one atomic paired motion command containing a target position, a target velocity, a stiffness gain, a damping gain, and a feed-forward torque, and the motor produces the resulting impedance behavior instead of tracking a velocity setpoint.

**Why this priority**: This is the core behavioral migration. Without it, nothing else in the feature has value, and the existing velocity-only interface cannot express compliant motion.

**Independent Test**: With the vehicle secured on a bench, send a matched command pair whose stiffness and damping are non-zero, confirm restoring and opposing effort, then prove a torn or sequence-mismatched pair changes no state and that each configured effort limit bounds output and reports its cause.

**Acceptance Scenarios**:

1. **Given** an armed, calibrated motor at rest, **When** a command arrives with a position target offset from the present position, non-zero stiffness, and zero feed-forward torque, **Then** the motor drives toward the position target with effort proportional to the position error.
2. **Given** an armed motor holding a position target, **When** the wheel is back-driven by hand, **Then** the motor opposes the motion with an effort that grows with both displacement and speed, rather than commanding a fixed velocity.
3. **Given** an armed motor, **When** a command arrives with zero stiffness, non-zero damping, and a velocity target, **Then** the motor behaves as a damped velocity follower, providing an impedance-mode equivalent of the previous velocity-control behavior.
4. **Given** an armed motor, **When** a command arrives with zero stiffness, zero damping, and a non-zero feed-forward torque, **Then** the motor produces that commanded torque independently of position and speed.
5. **Given** any impedance command, **When** the resulting effort reaches the configured current limit, output-voltage limit, or bus-voltage protection condition, **Then** the output stays within the safe bound and the correct per-motor cause is observable to the operator.
6. **Given** one half of an impedance command is missing, expired, malformed, or has a different sequence, **When** the controller evaluates the staged pair, **Then** no impedance term is applied, the command timeout is not refreshed, and the protocol fault is reported.

---

### User Story 2 - Deterministic sampling and control execution (Priority: P1)

As an integrator tuning the drive, I need current sampling and control computation to occur at a fixed, predictable rate derived from the configured current-loop bandwidth, so that the tuned gains produce the intended closed-loop response every run instead of drifting with processor load.

**Why this priority**: The impedance law and the current-loop gains are only meaningful if the loop executes at a known rate. The present controller runs the control loop opportunistically, so the realized bandwidth is unknown and load-dependent.

**Independent Test**: Run the controller at the default bandwidth with communications, telemetry, and operator traffic active, and record the measured interval between consecutive control cycles over a sustained run; confirm the interval stays within the stated tolerance and that no cycle is skipped.

**Acceptance Scenarios**:

1. **Given** a configured current-loop bandwidth, **When** the controller runs, **Then** current sampling and control computation execute at a fixed nominal rate determined by that bandwidth, not at a best-effort rate.
2. **Given** the controller is executing at its fixed rate, **When** communications, telemetry, operator commands, and voltage monitoring are all active, **Then** the control cycle interval remains within the stated jitter tolerance and no control cycle is dropped.
3. **Given** a control cycle cannot complete before its next scheduled start, **When** the overrun occurs, **Then** the controller records the overrun, keeps its timing reference, and surfaces the count to the operator.
4. **Given** sustained overruns exceed the configured tolerance, **When** the condition is detected, **Then** the controller fails closed by disarming all motors and reporting the timing fault.
5. **Given** two motors are driven by one controller, **When** a control cycle executes, **Then** both motors are sampled and updated within the same cycle so their control intervals are identical.

---

### User Story 3 - Configure the current-loop bandwidth (Priority: P2)

As an integrator, I can set the current-loop bandwidth anywhere from 100 Hz to 10000 Hz, with 1000 Hz used when I have not chosen a value, and the setting persists across power cycles and is applied consistently to the derived control gains and to the loop timing.

**Why this priority**: The bandwidth is the single tuning entry point for the migrated controller, but a working default must exist so a freshly flashed unit is usable without configuration.

**Independent Test**: Set several bandwidth values across the supported range, power cycle after each, and confirm the reported requested value, active value, derived gains, and nominal loop rate are consistent with each other and with any clamp; then confirm an unconfigured unit reports 1000 Hz.

**Acceptance Scenarios**:

1. **Given** no bandwidth has ever been configured, **When** the controller starts, **Then** it reports 1000 Hz as the requested default and uses either 1000 Hz or the lower measured sustainable active value with an explicit clamp report.
2. **Given** an operator sets a bandwidth within 100 Hz to 10000 Hz, **When** the value is accepted, **Then** the controller persists it and reports the resulting nominal loop rate and derived current-loop gains.
3. **Given** an operator requests a bandwidth inside the supported range that would need a switching carrier above the permissible maximum, **When** the request is evaluated, **Then** the requested value is stored, the active bandwidth is clamped to the highest achievable value, and both the requested and active values are reported along with the clamp.
4. **Given** an operator requests a bandwidth outside 100 Hz to 10000 Hz or a non-numeric value, **When** the request is evaluated, **Then** it is rejected, the previously stored value stays in effect, and the valid range is reported.
5. **Given** at least one motor is armed, **When** an operator requests a bandwidth change, **Then** the change is refused until the motors are disarmed, and the reason is reported.
6. **Given** a stored bandwidth is present but the motor's electrical characteristics are missing or invalid, **When** the controller starts, **Then** it does not enter normal impedance operation and reports that calibration is required.

---

### User Story 4 - Choose between impedance and velocity mode (Priority: P2)

As an integrator, I can select impedance mode or the retained velocity mode per motor, so that existing velocity-based senders keep working while new work moves to impedance control.

**Why this priority**: Both modes are permanently supported, so mode selection is part of the contract rather than a migration convenience. It must be explicit and safe, because the same payload bytes mean different things in each mode.

**Independent Test**: Select velocity mode and confirm the existing velocity command drives the wheel as before; select impedance mode and confirm the impedance command drives it; then send each mode's command against the other mode and confirm both are rejected.

**Acceptance Scenarios**:

1. **Given** a fresh controller or a motor whose stored mode is velocity, **When** the controller starts and an unmodified velocity sender commands motion after arming, **Then** the motor is in velocity mode and behaves exactly as it did before this feature.
2. **Given** a motor is disarmed, **When** the operator or sender selects velocity mode, **Then** the retained velocity command produces the previous velocity-following behavior and impedance frames for that motor are rejected.
3. **Given** a motor is disarmed, **When** impedance mode is selected, **Then** impedance frames are applied and legacy velocity frames for that motor are rejected and reported.
4. **Given** a motor is armed, **When** a mode change is requested, **Then** the change is refused, the active mode is unchanged, and the operator is told to disarm first.
5. **Given** a motor's mode changes while disarmed, **When** the change takes effect, **Then** all targets and gains are cleared so no effort carries across the mode boundary.

---

### User Story 5 - Fail safe when commands stop arriving (Priority: P2)

As an operator, I need a motor that is holding an impedance target to stop producing effort if commands stop arriving, so that a lost link or crashed sender cannot leave a wheel pushing against a stale position target.

**Why this priority**: Impedance control introduces a hazard that velocity control did not have in the same form: a stale position target with non-zero stiffness produces sustained torque indefinitely, including while the vehicle is stalled against an obstacle.

**Independent Test**: Arm a motor, send a position target with non-zero stiffness, then stop sending commands and confirm the motor stops producing effort within the stated timeout without operator intervention.

**Acceptance Scenarios**:

1. **Given** an armed motor acting on an impedance command, **When** no new motion command arrives for 50 ms, **Then** the controller zeroes commanded effort and reports the timeout.
2. **Given** an emergency stop is received, **When** the controller processes it, **Then** all commanded effort, stiffness, damping, and feed-forward terms are cleared and both motors are disarmed.
3. **Given** a motor is disarmed, **When** an impedance command arrives, **Then** no effort is produced and the stored targets do not carry over when the motor is next armed.

---

### User Story 6 - Observe realized timing and impedance state (Priority: P3)

As an integrator validating a build, I can read back the active bandwidth, the nominal and measured control rate, the overrun count, and the currently applied impedance terms, so that I can prove determinism instead of inferring it.

**Why this priority**: Determinism that cannot be measured cannot be verified during review or on-hardware sign-off, which the project's change gates require.

**Independent Test**: Query the controller during a sustained run and confirm the reported measured rate matches the nominal rate within tolerance and that overrun counters are visible and monotonic.

**Acceptance Scenarios**:

1. **Given** the controller is running, **When** the operator requests status, **Then** the requested and active bandwidth, active switching carrier, nominal control rate, measured control rate, worst observed cycle interval, and overrun count are reported.
2. **Given** the controller is running, **When** the operator requests status, **Then** each motor's active motion mode, applied position target, velocity target, stiffness, damping, feed-forward torque, and measured torque-producing current are reported.

---

### User Story 7 - Set impedance behavior from the operator interface (Priority: P3)

As an operator, I can choose the motion mode and adjust the stiffness and damping the vehicle drives with from the operator interface, so that tuning compliance does not require editing code or hand-crafting bus frames.

**Why this priority**: The impedance behavior is only usable day to day if an operator can pick a mode and change gains, but the vehicle is already drivable through the retained velocity mode without it.

**Independent Test**: From the operator interface, select impedance mode, set a stiffness and damping value, drive the vehicle, then change the stiffness and confirm the compliance felt at the wheel changes accordingly.

**Acceptance Scenarios**:

1. **Given** the operator interface is connected, **When** the operator selects impedance or velocity mode, **Then** the selection is sent to the affected controllers over the bus and the resulting active mode is displayed back from the controllers rather than assumed.
2. **Given** the operator interface is connected, **When** the operator views the current-loop bandwidth, **Then** it is shown as a read-only controller-reported value with no way to change it from the interface.
3. **Given** impedance mode is active, **When** the operator sets stiffness and damping within the supported ranges, **Then** subsequent motion commands use those gains for every driven wheel.
4. **Given** the operator enters a stiffness or damping value outside the supported range, **When** the value is submitted, **Then** it is refused with the valid range shown and the previously active gains stay in effect.
5. **Given** the vehicle is being driven, **When** the operator requests a mode change, **Then** the request follows the same disarm-first rule the controllers enforce, and the operator is told why if it is refused.
6. **Given** any operator interface state, **When** the operator triggers the emergency stop, **Then** it retains its existing precedence and effect regardless of the selected mode or gains.
7. **Given** an armed wheel is being driven with zero stiffness, **When** the operator selects non-zero stiffness, **Then** the controller captures that wheel's measured accumulated position in the same control cycle, the sender waits for the controller-reported applied target before holding it, and no stale cached position is used as the target.

### Edge Cases

- A requested bandwidth is valid per the 100 Hz to 10000 Hz range but requires a switching carrier above the present one: the controller raises the carrier to keep sampling deterministic, up to the maximum permissible carrier.
- A requested bandwidth needs a carrier beyond the hardware or current-sensing limit: the active bandwidth is clamped to what the maximum permissible carrier supports, the clamp is reported, and the requested value is still stored so it applies again if the carrier limit later increases.
- A control cycle overruns occasionally due to a burst of communication or persistence activity: isolated overruns are counted and tolerated without a timing-reference drift; sustained overruns disarm.
- The bandwidth is changed while a motor is armed or mid-motion: the change is refused until disarmed, so gains and loop rate never change under load.
- A position target implies a very large error, for example after a long communication gap or a sender that has lost track of the accumulated angle: the position-error term saturates at its documented maximum, and the resulting effort is further bounded by the current limit, voltage limit, bus-voltage protection, and command timeout rather than by the sender's discipline.
- The accumulated position has grown very large after prolonged driving: the signed 32-bit milliradian representation continues to cover at least 100 revolutions in either direction, and the controller reports the accumulated position over the bus so the sender can align subsequent targets.
- Only one half of a paired impedance command arrives, its sequence does not match, or the pair is malformed: the controller rejects the incomplete pair atomically, does not refresh the command timeout, retains the previous safe applied state, and reports the protocol fault.
- A command arrives with stiffness or damping outside the supported range, or with a malformed payload: the command is rejected and the previous safe state is retained.
- A motion frame arrives in a payload format that does not match the motor's active mode, for example a velocity frame while the motor is in impedance mode: the frame is rejected and reported, and no effort is produced from a reinterpreted payload.
- A mode change is requested while the motor is armed and moving: the change is refused until the motor is disarmed, so gains and command meaning never change under load.
- Only one motor's impedance frame arrives in a given interval: that motor's terms are applied on arrival and the other motor keeps its last valid terms until its own command timeout expires.
- Only one of the two motors on a controller is calibrated: normal impedance operation is unavailable for the uncalibrated motor and the controller reports which motor blocks operation.
- A sender that still uses the previous velocity-only command format addresses the controller after migration: the controller serves it in the retained velocity mode, and rejects the frame if the motor's active mode is impedance.
- The measured control rate diverges from the nominal rate while no overrun is recorded, indicating a timing-source fault: the controller reports the discrepancy and fails closed.
- The operator releases the controls or requests a normal stop: the sender continues at 200 Hz with explicit zero-effort commands, so the command timeout remains reserved for an actual link or sender failure.
- Emergency stop is engaged: the sender transmits `0x080` immediately and suspends all motion frames until emergency stop is cleared; controller status and heartbeat traffic may continue.
- A sender requests capture-current-position while enabling stiffness: each controller captures independently in its control cycle, reports the applied absolute target, and repeated capture requests remain damping-only rather than holding a stale sender estimate until confirmation arrives.

## Requirements *(mandatory)*

### Functional Requirements

#### Impedance control behavior

- **FR-001**: The controller MUST compute each motor's commanded torque from a position target, a velocity target, a stiffness gain, a damping gain, and a feed-forward torque term, using the measured position and measured velocity of that motor.
- **FR-001a**: The position target and measured position MUST both be the absolute accumulated shaft angle since power-up. The bus representation MUST encode the target as a signed 32-bit integer in milliradians, covering at least 100 revolutions in either direction without wrapping to a single turn.
- **FR-001b**: The controller MUST saturate the position-error term at a documented maximum magnitude before it is multiplied by stiffness, so that an arbitrarily large position error cannot request an unbounded torque.
- **FR-001c**: The controller MUST report each motor's measured absolute accumulated position over the bus, so a sender can align its targets with the controller's power-up zero without guessing.
- **FR-002**: The controller MUST accept commands that set all five impedance terms per motor independently for motor 1 and motor 2 using an atomic matched pair consisting of one position frame and one dynamics frame.
- **FR-003**: The controller MUST support the degenerate cases of the impedance law: zero stiffness with non-zero damping and a velocity target MUST reproduce damped velocity-following behavior, and zero stiffness with zero damping MUST reproduce direct torque control.
- **FR-004**: The controller MUST convert the commanded torque into a current setpoint for the existing torque-producing current loop, MUST limit that setpoint to the configured per-motor current limit, and MUST preserve the configured output-voltage limit and bus-voltage protection before effort is applied.
- **FR-004a**: The controller MUST record whether effort was limited by the current limit, output-voltage limit, or bus-voltage protection and MUST expose the active cause on the serial console and over the bus without adding diagnostic work to the periodic control path.
- **FR-005**: The controller MUST document and enforce a valid range for each impedance term, and MUST reject a command containing any term outside its range without applying any part of that command.
- **FR-006**: The controller MUST support impedance control as the primary motion mode while permanently retaining the existing velocity-target mode as a supported second mode, so a sender may use either.
- **FR-006a**: The controller MUST hold and persist an explicit active motion mode per motor, MUST default a fresh or invalid configuration to velocity, MUST restore the last accepted valid mode on startup, and MUST reject a motion command whose payload does not match the restored active mode.
- **FR-006b**: The controller MUST zero all commanded effort, targets, and gains for a motor when that motor's active motion mode changes, MUST refuse a mode change while that motor is armed, and MUST persist an accepted mode change before reporting it successful.
- **FR-006d**: Regardless of the restored mode, every startup MUST begin disarmed with zero effort, targets, gains, and pending command halves; restoring impedance mode MUST NOT restore any prior motion command.
- **FR-006c**: The controller MUST apply the same safety rules to both motion modes, including current and voltage limiting, emergency stop precedence, arm and disarm semantics, and the command timeout.
- **FR-007**: The controller MUST accept one atomic two-frame impedance command per motor: a position frame carrying a signed 32-bit absolute accumulated-position target in milliradians and a dynamics frame carrying velocity, stiffness, damping, and feed-forward torque. Each motor and each half MUST use a distinct frame identifier on each node.
- **FR-007a**: Both halves of an impedance command MUST carry the same sequence number. The controller MUST stage the halves and apply all five terms atomically only after a valid matching pair arrives; an incomplete, stale, mismatched, or malformed pair MUST NOT partially update state or refresh the command timeout. Arm, disarm, emergency-stop, and mode selection MUST remain on a separate control frame.
- **FR-007e**: The dynamics half MUST provide a capture-current-position flag. When set on a valid matched pair, the controller MUST replace the transmitted position target with the measured accumulated position from that same control cycle before applying non-zero stiffness.
- **FR-007b**: The controller MUST document identifiers, field order, field widths, scaling, signedness, and the motor-to-wheel mapping for every new and changed frame in the same change that introduces it.
- **FR-007c**: The controller MUST assemble and apply each motor's matched pair independently, and MUST NOT delay one motor's complete pair while waiting for the other motor's pair.
- **FR-007d**: The controller MUST accept motion-mode selection over the bus and MUST report each motor's resulting active mode over the bus, so a sender can confirm the mode it is commanding in.
- **FR-008**: The controller MUST clear all impedance terms to a zero-effort state when a motor is disarmed, when an emergency stop is received, and when the controller enters calibration mode.
- **FR-009**: The controller MUST NOT carry impedance terms received while disarmed into the armed state; arming MUST begin from a zero-effort state.

#### Deterministic sampling and control

- **FR-010**: The controller MUST execute current sampling and control computation on a fixed periodic schedule driven by a hardware time base, and MUST NOT depend on best-effort scheduling or cooperative yielding for its control period.
- **FR-011**: The controller MUST derive the nominal sampling and control rate from the active current-loop bandwidth using a documented, fixed sampling multiple of at least ten and MUST NOT reduce that multiple to make a requested bandwidth appear reachable.
- **FR-012**: The controller MUST take exactly one current measurement per motor per control cycle and MUST use measurements from the same cycle for both motors' control updates.
- **FR-013**: The controller MUST update both motors' outputs within the same control cycle so that both motors share an identical control interval.
- **FR-014**: The controller MUST keep its periodic timing reference independent of individual cycle execution time, so that a late cycle does not shift the phase of subsequent cycles.
- **FR-015**: The controller MUST detect and count control-cycle overruns, MUST record the worst observed cycle interval, and MUST expose both to the operator.
- **FR-016**: The controller MUST disarm all motors and report the cause when overruns exceed a documented sustained-overrun threshold, or when the measured control rate deviates from the nominal rate beyond a documented tolerance.
- **FR-017**: The controller MUST give the periodic control work precedence over communications, telemetry, operator command handling, voltage monitoring, and persistence work.
- **FR-018**: The controller MUST keep the switching carrier and the control period in a documented fixed relationship so that the sampling instant is repeatable relative to the switching cycle.
- **FR-019**: The controller MUST compute time-dependent control terms from the fixed nominal control period rather than from a measured elapsed time, once determinism is established.
- **FR-020**: When the active bandwidth requires a sampling rate above what the present switching carrier supports, the controller MUST raise the switching carrier to the frequency required to keep the sampling rate deterministic, up to the documented hardware and current-sensing limits.
- **FR-020a**: The controller MUST verify that current sensing remains valid at the raised switching carrier, and MUST report the active switching carrier alongside the active bandwidth and control rate.
- **FR-020b**: When a requested bandwidth inside the supported range would require a switching carrier above the documented hardware or current-sensing limit, the controller MUST clamp the active bandwidth to the highest value that the maximum permissible carrier supports, MUST report that the request was clamped along with the resulting active value, and MUST NOT run any active bandwidth at a slower, non-deterministic rate.
- **FR-020c**: The controller MUST NOT change the switching carrier while any motor is armed.
- **FR-020d**: The controller MUST persist the requested bandwidth rather than the clamped value, MUST re-derive the active value on every startup and on every carrier-limit change, and MUST report the requested and active values separately whenever they differ.

#### Bandwidth configuration

- **FR-021**: The controller MUST expose the current-loop bandwidth as a user-configurable parameter accepting values from 100 Hz through 10000 Hz inclusive.
- **FR-021a**: The controller MUST accept bandwidth changes only from the serial operator console, and MUST reject any attempt to change the bandwidth over the bus.
- **FR-021b**: The controller MUST report the requested bandwidth, the active bandwidth, and the active switching carrier over the bus so the operator interface can display them without being able to change them.
- **FR-021c**: The bandwidth reported over the bus MUST distinguish a clamped active value from the requested value, so the operator interface can show that a request was reduced.
- **FR-022**: The controller MUST use 1000 Hz as the requested bandwidth when no valid configured value is stored, then derive and report the active bandwidth using the same measured ceiling and clamp rules as any other request.
- **FR-023**: The controller MUST persist an accepted bandwidth value and restore it on startup.
- **FR-024**: The controller MUST reject an out-of-range, malformed, or non-numeric bandwidth request, retain the previously stored value, and report the accepted range.
- **FR-025**: The controller MUST refuse a bandwidth change while any motor is armed, and MUST report that the motors must be disarmed first.
- **FR-026**: The controller MUST recompute the derived current-loop gains and filter settings from the stored motor electrical characteristics whenever the active bandwidth changes, and MUST report the resulting values.
- **FR-027**: The controller MUST refuse to enter normal impedance operation when the required per-motor electrical characteristics are missing or invalid, and MUST report that calibration is required.
- **FR-028**: The controller MUST apply the same active bandwidth to both motors on a node.

#### Safety

- **FR-029**: The controller MUST zero all commanded effort for a motor when no valid motion command for that motor has been received within 50 ms, and MUST report the timeout.
- **FR-029a**: The controller MUST apply the command timeout independently per motor, so a motor whose frames have stopped goes quiet even while the other motor is still being commanded.
- **FR-029b**: The controller MUST resume acting on commands without operator intervention once valid commands resume after a timeout, provided the motor is still armed.
- **FR-030**: The controller MUST continue to honor the emergency-stop path with higher precedence than any impedance command, and MUST reach a zero-output state on emergency stop regardless of the configured bandwidth or loop rate.
- **FR-031**: The controller MUST preserve the existing arm and disarm semantics, including the requirement that a disarm leaves the output stage unpowered and targets zeroed.
- **FR-032**: The controller MUST bound commanded effort by the existing per-motor current limit, configured output-voltage limit, and bus-voltage protection independently of the impedance terms supplied by the sender, and each protection path MUST be verified in impedance mode.
- **FR-032a**: When any effort limit activates, the controller MUST keep the applied output within the configured bound, latch the per-motor limit cause long enough for communications code to report it, and make the event visible to the operator within one second.

#### Observability

- **FR-033**: The controller MUST report the requested and active bandwidth, sampling multiple, active switching carrier, nominal control rate, measured control rate, worst cycle interval, and overrun count on the serial console on operator request.
- **FR-034**: The controller MUST report each motor's active motion mode, currently applied impedance terms, measured position, measured velocity, and measured torque-producing current on the serial console on operator request.
- **FR-034a**: The controller MUST report over the bus at least the per-motor active mode, applied position target, applied stiffness and damping, active bandwidth, and timeout or overrun state, so the sender and operator interface can use controller-confirmed values.
- **FR-035**: The controller MUST keep diagnostic output out of the periodic control path so that reporting cannot cause control-cycle overruns.

#### Sender and operator interface

- **FR-036**: The sender MUST be able to emit an atomic, sequence-matched position-and-dynamics frame pair for every driven wheel, and MUST continue to support emitting the retained velocity frames.
- **FR-037**: The sender MUST keep performing vehicle-level mixing, producing per-wheel targets from the operator's motion input in whichever mode is active.
- **FR-038**: The sender MUST emit motion commands for each driven motor at a nominal 200 Hz during continuous motion and MUST continue at 200 Hz with explicit zero-effort commands during normal idle or stop. It MUST suspend motion frames while emergency stop is engaged and MUST go silent only when prevented by a link or sender failure, allowing the 50 ms controller timeout to detect that failure.
- **FR-038b**: On emergency stop, the sender MUST transmit the existing `0x080` emergency-stop frame immediately before suspending motion frames, and MUST NOT resume motion frames until emergency stop has been cleared through the existing safety path.
- **FR-038a**: The doubled impedance frame count MUST leave bus headroom for the existing status and heartbeat traffic at the configured bus speed, and the resulting worst-case bus utilization MUST be documented and remain below 50% under nominal operation.
- **FR-039**: The operator interface MUST let an operator select the motion mode and set the stiffness and damping used for driving, and MUST validate entries against the supported ranges before sending them.
- **FR-039a**: When an armed motor transitions from zero to non-zero stiffness, the sender MUST request controller-local position capture, MUST NOT derive the hold target from a cached position report, and MUST continue requesting capture without assuming success until the controller reports the applied target; only then may it send that confirmed absolute target without the capture flag.
- **FR-040**: The operator interface MUST display the mode, gains, and bandwidth reported back by the controllers rather than only the values it requested, so a refused or clamped setting is visible.
- **FR-040a**: The operator interface MUST present the bandwidth as read-only, because bandwidth changes are accepted only from the serial console.
- **FR-041**: The operator interface MUST preserve the existing emergency-stop control and its precedence, immediately trigger the `0x080` sender path, and keep motion transmission suspended while emergency stop is engaged, unchanged by the selected mode or gains.
- **FR-042**: The protocol change MUST be applied to firmware, sender, and operator interface together, and the frame documentation MUST be updated in the same change.

### Key Entities

- **Impedance command**: A per-motor atomic motion request delivered as a sequence-matched position-and-dynamics frame pair. The position half carries a signed 32-bit absolute accumulated shaft angle in milliradians; the dynamics half carries velocity, stiffness, damping, and feed-forward torque.
- **Motion mode**: The per-motor selection of impedance or velocity control, which determines how an incoming motion payload is interpreted and which payloads are rejected.
- **Current-loop configuration**: The persisted, user-configurable bandwidth together with the derived sampling multiple, nominal control rate, active switching carrier, and derived current-loop gains and filter settings.
- **Loop timing record**: The runtime measurement set describing realized determinism: measured control rate, worst observed cycle interval, overrun count, and sustained-overrun state.
- **Motor calibration record**: The existing per-motor electrical and sensor data, which supplies the resistance and inductance used to derive current-loop gains for the active bandwidth.
- **Motor runtime state**: The per-motor armed state, active motion mode, applied impedance or velocity terms, last command time, measured position, measured velocity, and measured torque-producing current.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: At the active bandwidth derived from the default 1000 Hz requested setting, with communications, telemetry, and operator traffic active, at least 99.9% of control cycles start within 5% of the nominal period, and no cycle is skipped, over a 10-minute continuous run.
- **SC-002**: Over a 10-minute continuous run at the active bandwidth derived from the default requested setting, the measured control rate matches the nominal control rate within 1%, and the overrun count remains zero.
- **SC-003**: Repeating the same commanded impedance step three times, under idle load and under full communication load, produces wheel responses whose settling behavior agrees within 10%, demonstrating load-independent control timing.
- **SC-004**: A held position target with non-zero stiffness produces zero commanded effort within 50 ms of commands stopping, in 100 consecutive trials.
- **SC-004a**: With commands streaming at the nominal 200 Hz, dropping up to nine consecutive frames does not interrupt motion, and dropping more than that zeroes effort within the timeout.
- **SC-005**: An emergency stop reaches a zero-output, disarmed state in 100 consecutive trials at both the lowest and the highest supported bandwidth settings.
- **SC-006**: A pure software check exhaustively validates every integer bandwidth request from 100 through 10000 Hz plus malformed and out-of-range inputs for acceptance, derivation, and clamp invariants. On hardware, each unique value in the fixed matrix 100, 500, 1000, measured sustainable ceiling, ceiling plus one, and 10000 Hz survives a power cycle as the unchanged requested value, reports the correct active/clamped result, and leaves the stored value unchanged after invalid input.
- **SC-007**: A back-driven wheel under a non-zero stiffness command produces a restoring effort that is monotonic in displacement across at least five test displacements, with no commanded current exceeding the configured limit.
- **SC-007a**: In impedance mode, deliberate current-limit, output-voltage-limit, and bus-voltage-protection conditions keep the applied output within the configured bound in five consecutive trials per protection path, and the correct per-motor limit cause appears on serial, CAN, and the operator interface within one second.
- **SC-008**: Zero-stiffness, non-zero-damping commands reproduce the previous velocity-following behavior within 10% steady-state speed error across the operating speed range, so the vehicle remains drivable after migration.
- **SC-009**: An operator can read the requested and active bandwidth, active switching carrier, nominal and measured control rate, and overrun count in a single serial status request.
- **SC-009a**: 100% of attempts to change the bandwidth over the bus are rejected without altering the stored or active value.
- **SC-010**: With all four motors' stored modes set to velocity, an unmodified velocity-based sender drives the vehicle with no observable behavior change from before this feature across forward, reverse, strafe, and rotation; after a power cycle, each motor also restores whichever valid mode was last accepted while remaining disarmed and at zero effort.
- **SC-011**: In 100 trials of sending a payload that does not match a motor's active mode, 100% are rejected and none produce motor effort.
- **SC-012**: For every integer request from 100 through 10000 Hz, the pure derivation check proves that the active bandwidth, carrier, decimation, and nominal control rate satisfy the fixed sampling multiple and clamp invariants. Hardware phase-current readings at the lowest, default-derived, and highest active carrier points remain within 5% of the reference measurement at the default-derived configuration.
- **SC-013**: A signed 32-bit milliradian position target deliberately offset by 100 revolutions, delivered as a valid matched frame pair at maximum supported stiffness, produces no more commanded current than the same stiffness at the documented position-error saturation limit in 100 consecutive trials; incomplete or sequence-mismatched pairs produce no partial state change.
- **SC-014**: An operator can switch motion mode and change the driving stiffness and damping from the operator interface without editing code or sending bus frames by hand; enabling stiffness on an armed moving wheel captures the same-cycle controller position without a saturated effort transient, and the interface shows the applied target and other controller-reported values within 1 second, with bandwidth shown read-only.
- **SC-015**: Driving the vehicle in impedance mode through the operator interface reproduces forward, reverse, strafe, and rotation with correct wheel directions, matching the behavior observed in velocity mode.

## Assumptions

- The impedance law is the MIT-style form used by the Mini Cheetah actuator interface: commanded torque equals stiffness times position error plus damping times velocity error plus feed-forward torque.
- Accumulated position starts at zero on power-up rather than at a mechanical home, because a drive wheel has no home position; senders that need position holds are expected to read the reported reference rather than assume a shared origin.
- The position-error saturation limit is a new safety parameter; its value is set during planning from the wheel's usable stiffness range and the per-motor current limit.
- The existing calibration feature remains the source of per-motor resistance, inductance, alignment, and sensor direction, and remains a prerequisite for normal operation; this feature does not change calibration procedures other than replacing the fixed 100 Hz bandwidth with the configurable value.
- The existing emergency-stop identifier, arm and disarm commands, status reporting identifiers, and heartbeat behavior are retained; the impedance interface is added alongside the retained velocity interface.
- Bandwidth stays a bench commissioning parameter on the serial console alongside calibration, which is already serial-only; the bus carries mode selection and reporting but never a bandwidth write.
- Because each motor receives a two-frame impedance command, a four-wheel vehicle sends eight motion frames per update instead of two; at the nominal 200 Hz rate on the existing 1 Mbit/s bus the measured worst-case utilization must remain below 50% with status and heartbeat traffic active.
- A maximum permissible switching carrier exists, bounded by the driver's and motors' thermal and switching limits and by current-sensing validity; the plan phase establishes its value, and it in turn sets the highest reachable bandwidth.
- The current-sensing method is assumed to remain valid at raised carriers only if its sampling window still fits the shorter switching period; verifying this is part of accepting a bandwidth.
- The sender remains responsible for vehicle-level mixing; the controller applies the impedance law per motor and does not perform wheel mixing.
- Stiffness and damping are vehicle-level settings applied to all driven wheels rather than per-wheel operator inputs; per-wheel gain tuning is out of scope.
- The two motors on a node share one control cycle and one bandwidth setting; per-motor bandwidth is out of scope.
- The firmware, the Jetson backend sender, and the Vue operator UI are all in scope for this feature and are changed together, so the protocol boundary does not silently drift, consistent with the project's protocol-compatibility requirement.
- The command timeout is a new safety parameter introduced by this feature because impedance control can produce sustained effort from a stale target; it is fixed at 50 ms against a nominal 200 Hz command rate.
- The sampling multiple is never less than ten times the active bandwidth. Hardware measurements may lower the active bandwidth through the existing clamp rule but never lower this sampling multiple.
- Both motion modes are permanently supported, so the plan and tasks must budget validation for both rather than treating velocity mode as deprecated.
- Velocity mode is the default for a fresh or invalid configuration, while later accepted per-motor mode selections persist across power cycles. Startup safety comes from every motor being disarmed with all motion state zeroed, regardless of the restored mode.
- Mecanum kinematics and the existing mixing behavior are unchanged; the operator interface gains mode and gain controls but its existing driving and emergency-stop interactions stay as they are.
- Validation is performed with the vehicle secured on a bench before any on-vehicle run, consistent with the project's safety-critical motion gates.
