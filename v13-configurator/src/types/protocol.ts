export type ConnectionState = 'disconnected' | 'identifying' | 'ready' | 'busy' | 'lost'

export interface IdRecord {
  firmware_level: string
  protocol_version: number
  can_id: number
  motor_count: number
  config_version: number
  uptime_ms: number
}

export interface CalRecord {
  motor: number
  aligned: boolean
  characterised: boolean
  pole_pairs: number
  direction: number
  electrical_offset: number
  phase_resistance: number
  inductance_d: number
  inductance_q: number
  valid: boolean
}

export interface CfgRecord {
  can_id: number
  bandwidth_requested_hz: number
  bandwidth_active_hz: number
  bandwidth_clamped: boolean
  control_rate_hz: number
  carrier_hz: number
  decimation: number
  mode: Array<'velocity' | 'impedance'>
  bus_min_mv: number
  bus_max_mv: number
  calibrated: boolean
}

export interface MotorRecord {
  motor: number
  armed: boolean
  mode: 'velocity' | 'impedance'
  position_mrad: number
  velocity: number
  current_q: number
  timed_out: boolean
  limit_causes: {
    current: boolean
    output_voltage: boolean | null
    bus_voltage: boolean
  }
  limit_count: number
  pair_fault: boolean | null
}

export interface ImpRecord {
  motor: number
  p_des_mrad: number
  v_des: number
  kp: number
  kd: number
  t_ff: number
  position_error: number
  torque_cmd: number
  applied_target_mrad: number
  capture_generation: number
  last_seq: number
  pair_fault: boolean | null
  eligible: boolean
  serial_hold: boolean
}

export interface TimingRecord {
  rate_nominal_hz: number
  rate_measured_hz: number
  period_us: number
  cycles: number
  overruns: number
  consecutive_overruns: number
  last_cycle_us: number
  worst_cycle_us: number
  duty_percent: number
  fault: 'none' | 'overrun' | 'rate'
}

export interface BusRecord {
  millivolts: number
  protection_active: boolean
}

export interface DeviceMirror {
  cal: CalRecord[]
  cfg: CfgRecord | null
  motors: MotorRecord[]
  impedance: ImpRecord[]
  timing: TimingRecord | null
  bus: BusRecord | null
}

export interface ControllerSnapshot {
  identity: IdRecord | null
  mirror: DeviceMirror
}

export interface ConnectResult {
  identity: IdRecord
  reset_forced: boolean
}

export interface PortDescriptor {
  name: string
  port_type: string
}

export interface CalProgRecord {
  motor: number
  stage: 'align' | 'charac'
  percent: number
  energised: boolean
}

export interface CalPendRecord {
  motor: number
  stage: 'align' | 'charac'
  pole_pairs?: number | null
  direction?: number | null
  electrical_offset?: number | null
  phase_resistance?: number | null
  inductance_d?: number | null
  inductance_q?: number | null
}

export interface FaultRecord {
  kind: string
  reason: string
  cooldown_ms: number
}

export interface TelemetryResult {
  applied_period_ms: number
}

export interface SessionError {
  kind:
    | 'not_connected'
    | 'refused'
    | 'timeout'
    | 'protocol'
    | 'port_lost'
    | 'busy'
    | 'needs_confirmation'
  reason: string
}
