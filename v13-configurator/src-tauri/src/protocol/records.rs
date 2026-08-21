//! Typed representations of every structured serial record.
//!
//! `pairfault` and the output-voltage limit cause stay `Option` so a board still flashing the
//! older firmware (`pairfault=nan`) is shown as unavailable rather than as a confident negative.
//! Current firmware evaluates both and emits `0` or `1`.

use serde::Serialize;

/// Motion mode as reported by the controller.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum MotionMode {
    Velocity,
    Impedance,
}

impl MotionMode {
    pub fn from_code(code: u8) -> Option<Self> {
        match code {
            0 => Some(MotionMode::Velocity),
            1 => Some(MotionMode::Impedance),
            _ => None,
        }
    }
}

/// Effort-limit causes, decoded from the reported bitmask.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize)]
pub struct LimitCauses {
    pub current: bool,
    /// `None` only when the firmware still emits `pairfault=nan` (pre-evaluation boards).
    pub output_voltage: Option<bool>,
    pub bus_voltage: bool,
}

impl LimitCauses {
    pub const BIT_CURRENT: u32 = 1 << 0;
    pub const BIT_OUTPUT_VOLTAGE: u32 = 1 << 1;
    pub const BIT_BUS_VOLTAGE: u32 = 1 << 2;

    /// `output_voltage_evaluated` reflects whether the firmware actually computes that cause yet.
    pub fn from_mask(mask: u32, output_voltage_evaluated: bool) -> Self {
        Self {
            current: mask & Self::BIT_CURRENT != 0,
            output_voltage: if output_voltage_evaluated {
                Some(mask & Self::BIT_OUTPUT_VOLTAGE != 0)
            } else {
                None
            },
            bus_voltage: mask & Self::BIT_BUS_VOLTAGE != 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct IdRecord {
    pub firmware_level: String,
    pub protocol_version: u16,
    pub can_id: u16,
    pub motor_count: u8,
    pub config_version: u16,
    pub uptime_ms: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CalRecord {
    pub motor: u8,
    pub aligned: bool,
    pub characterised: bool,
    pub pole_pairs: u16,
    /// `1` clockwise, `-1` counter-clockwise.
    pub direction: i8,
    pub electrical_offset: f32,
    pub phase_resistance: f32,
    pub inductance_d: f32,
    pub inductance_q: f32,
    /// The controller's own validity verdict, not ours.
    pub valid: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CfgRecord {
    pub can_id: u16,
    pub bandwidth_requested_hz: u16,
    pub bandwidth_active_hz: u16,
    pub bandwidth_clamped: bool,
    pub control_rate_hz: u32,
    pub carrier_hz: u32,
    pub decimation: u8,
    pub mode: [MotionMode; 2],
    pub bus_min_mv: u16,
    pub bus_max_mv: u16,
    pub calibrated: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct MotorRecord {
    pub motor: u8,
    pub armed: bool,
    pub mode: MotionMode,
    pub position_mrad: i32,
    pub velocity: f32,
    pub current_q: f32,
    pub timed_out: bool,
    pub limit_causes: LimitCauses,
    pub limit_count: u16,
    /// `None` only when the firmware still emits `nan` (CAN pair RX not compiled in on that flash).
    pub pair_fault: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ImpRecord {
    pub motor: u8,
    pub p_des_mrad: i32,
    pub v_des: f32,
    pub kp: f32,
    pub kd: f32,
    pub t_ff: f32,
    pub position_error: f32,
    pub torque_cmd: f32,
    pub applied_target_mrad: i32,
    pub capture_generation: u8,
    /// `-1` when no pair or serial apply has been accepted yet.
    pub last_seq: i32,
    pub pair_fault: Option<bool>,
    pub eligible: bool,
    pub serial_hold: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum TimingFault {
    None,
    Overrun,
    Rate,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct TimingRecord {
    pub rate_nominal_hz: u32,
    pub rate_measured_hz: f32,
    pub period_us: f32,
    pub cycles: u32,
    pub overruns: u32,
    pub consecutive_overruns: u16,
    pub last_cycle_us: u32,
    pub worst_cycle_us: u32,
    pub duty_percent: f32,
    pub fault: TimingFault,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct BusRecord {
    pub millivolts: u32,
    pub protection_active: bool,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct AckRecord {
    pub tag: u8,
    pub command: String,
    pub ok: bool,
    /// The controller's own wording, already percent-decoded. Empty on success.
    pub reason: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum CalStage {
    Align,
    Charac,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CalProgRecord {
    pub motor: u8,
    pub stage: CalStage,
    pub percent: u8,
    pub energised: bool,
}

/// A stage result awaiting an accept or reject decision. Which fields are present depends on the
/// stage: alignment yields pole pairs, direction and offset; characterisation yields R and L.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CalPendRecord {
    pub motor: u8,
    pub stage: CalStage,
    pub pole_pairs: Option<u16>,
    pub direction: Option<i8>,
    pub electrical_offset: Option<f32>,
    pub phase_resistance: Option<f32>,
    pub inductance_d: Option<f32>,
    pub inductance_q: Option<f32>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum FaultKind {
    Calibration,
    Timing,
    Bus,
    Protocol,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct FaultRecord {
    pub kind: FaultKind,
    pub reason: String,
    pub cooldown_ms: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum Record {
    Id(IdRecord),
    Cal(CalRecord),
    Cfg(CfgRecord),
    Motor(MotorRecord),
    Imp(ImpRecord),
    Timing(TimingRecord),
    Bus(BusRecord),
    Ack(AckRecord),
    CalProg(CalProgRecord),
    CalPend(CalPendRecord),
    Fault(FaultRecord),
}
