pub mod calibration;
pub mod error;
pub mod identity;
pub mod intent;
pub mod port;
pub mod state;

pub use calibration::{CalibrationPhase, CalibrationSession};
pub use error::{ErrorKind, SessionError};
pub use identity::wheel_label;
pub use intent::IntentToken;
pub use port::{list_ports, open_exclusive, reset_forced_from_identity, PortDescriptor};
pub use state::{ConnectionState, InFlight, SessionState};

use crate::protocol::{
    encode_abort, encode_command, parse_line, AckRecord, BusRecord, CalRecord, CfgRecord, IdRecord,
    ImpRecord, MotorRecord, ParseOutcome, Record, TimingRecord,
};
use serde::Serialize;
use std::time::{Duration, Instant};

pub const NORMAL_TIMEOUT_MS: u64 = 2_000;
pub const CALIBRATION_TIMEOUT_MS: u64 = 60_000;
pub const IDENTIFY_TIMEOUT: Duration = Duration::from_millis(NORMAL_TIMEOUT_MS);
pub const SUPPORTED_PROTOCOL_VERSION: u16 = 1;
pub const SUPPORTED_CONFIG_VERSIONS: &[u16] = &[1, 2];
pub const EXPECTED_MOTOR_COUNT: u8 = 2;
pub const STALE_AFTER: Duration = Duration::from_secs(2);
pub const QP_MIN_PERIOD_MS: u32 = 50;

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "event", rename_all = "snake_case")]
pub enum SessionEvent {
    ConnectionChanged {
        state: ConnectionState,
        identity: Option<IdRecord>,
        reason: Option<String>,
        reset_forced: bool,
    },
    Telemetry {
        record: Record,
    },
    CalibrationProgress {
        record: crate::protocol::CalProgRecord,
    },
    CalibrationPending {
        record: crate::protocol::CalPendRecord,
    },
    Fault {
        record: crate::protocol::FaultRecord,
    },
    ProtocolError {
        detail: String,
    },
    Staleness {
        last_update_age_ms: u64,
    },
}

#[derive(Debug, Clone, Serialize)]
pub struct ConnectResult {
    pub identity: IdRecord,
    pub reset_forced: bool,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct DeviceMirror {
    pub cal: Vec<CalRecord>,
    pub cfg: Option<CfgRecord>,
    pub motors: Vec<MotorRecord>,
    pub impedance: Vec<ImpRecord>,
    pub timing: Option<TimingRecord>,
    pub bus: Option<BusRecord>,
}

pub struct Session {
    pub state: SessionState,
    pub mirror: DeviceMirror,
    pub last_ack: Option<AckRecord>,
    pub intent: Option<IntentToken>,
    pub calibration: Option<CalibrationSession>,
    pub telemetry_period_ms: u32,
    pub write_unknown: bool,
    pub last_telemetry_at: Option<Instant>,
    stale_emitted: bool,
    line_buf: String,
    identify_deadline: Option<Instant>,
    reset_forced: bool,
}

impl Default for Session {
    fn default() -> Self {
        Self {
            state: SessionState::default(),
            mirror: DeviceMirror::default(),
            last_ack: None,
            intent: None,
            calibration: None,
            telemetry_period_ms: 0,
            write_unknown: false,
            last_telemetry_at: None,
            stale_emitted: false,
            line_buf: String::new(),
            identify_deadline: None,
            reset_forced: false,
        }
    }
}

impl Session {
    pub fn begin_connect(&mut self, port_name: String, now: Instant) {
        self.state = SessionState::default();
        self.state.port_name = Some(port_name);
        self.state.transition(ConnectionState::Identifying);
        self.identify_deadline = Some(now + IDENTIFY_TIMEOUT);
        self.reset_forced = false;
        self.intent = None;
        self.calibration = None;
        self.write_unknown = false;
        self.line_buf.clear();
    }

    pub fn disconnect(&mut self) {
        self.state.transition(ConnectionState::Disconnected);
        self.identify_deadline = None;
        self.line_buf.clear();
        self.reset_forced = false;
    }

    pub fn mark_lost(&mut self, _reason: impl Into<String>) {
        self.state.transition(ConnectionState::Lost);
        self.identify_deadline = None;
        self.intent = None;
        if let Some(cal) = &mut self.calibration {
            cal.interrupt();
        }
        self.write_unknown = true;
        self.state.mirrored_data_stale = true;
    }

    /// Encode a tagged command. Abort is the only request allowed while `busy`.
    pub fn prepare_request(&mut self, command: &str, now: Instant) -> Result<Vec<u8>, SessionError> {
        match self.state.connection {
            ConnectionState::Ready => {}
            ConnectionState::Busy => {
                return Err(SessionError::new(
                    ErrorKind::Busy,
                    "another request is in flight; only abort is permitted",
                ));
            }
            ConnectionState::Disconnected | ConnectionState::Lost | ConnectionState::Identifying => {
                return Err(SessionError::new(
                    ErrorKind::NotConnected,
                    "no identified session",
                ));
            }
        }
        if self.state.in_flight.is_some() {
            return Err(SessionError::new(
                ErrorKind::Busy,
                "another request is in flight; only abort is permitted",
            ));
        }
        let tag = self.state.allocate_tag();
        let bytes = encode_command(tag, command)
            .map_err(|e| SessionError::new(ErrorKind::Protocol, e.to_string()))?;
        let timeout = timeout_for(command);
        self.state.in_flight = Some(InFlight {
            tag,
            command: command.to_string(),
            deadline: now + timeout,
        });
        self.state.connection = ConnectionState::Busy;
        Ok(bytes)
    }

    /// Raw abort byte. Never queued; permitted in `busy` and `ready`.
    pub fn prepare_abort(&mut self) -> Result<[u8; 1], SessionError> {
        match self.state.connection {
            ConnectionState::Disconnected | ConnectionState::Identifying => {
                return Err(SessionError::new(
                    ErrorKind::NotConnected,
                    "no session to abort",
                ));
            }
            ConnectionState::Ready | ConnectionState::Busy | ConnectionState::Lost => {}
        }
        Ok(encode_abort())
    }

    pub fn ingest_bytes(&mut self, bytes: &[u8], now: Instant) -> Vec<SessionEvent> {
        let mut events = Vec::new();
        for b in bytes {
            if *b == b'\n' {
                let line = std::mem::take(&mut self.line_buf);
                events.extend(self.ingest_line(&line, now));
            } else if *b != b'\r' {
                self.line_buf.push(*b as char);
            }
        }
        events
    }

    pub fn ingest_line(&mut self, line: &str, now: Instant) -> Vec<SessionEvent> {
        let mut events = Vec::new();
        match parse_line(line) {
            ParseOutcome::Ignored => {}
            ParseOutcome::Error(err) => {
                events.push(SessionEvent::ProtocolError {
                    detail: format!("{err:?}"),
                });
            }
            ParseOutcome::Record(Record::Id(id)) => events.extend(self.on_id(id)),
            ParseOutcome::Record(Record::Ack(ack)) => events.extend(self.on_ack(ack)),
            ParseOutcome::Record(Record::CalProg(record)) => {
                events.push(SessionEvent::CalibrationProgress { record });
            }
            ParseOutcome::Record(Record::CalPend(record)) => {
                events.push(SessionEvent::CalibrationPending { record });
            }
            ParseOutcome::Record(Record::Fault(record)) => {
                events.push(SessionEvent::Fault { record });
            }
            ParseOutcome::Record(record) => {
                self.update_mirror(&record);
                self.note_telemetry(now);
                if self.state.connection == ConnectionState::Ready
                    || self.state.connection == ConnectionState::Busy
                {
                    events.push(SessionEvent::Telemetry { record });
                }
            }
        }
        if let Some(event) = self.poll_timeouts(now) {
            events.push(event);
        }
        events
    }

    pub fn note_telemetry(&mut self, now: Instant) {
        self.last_telemetry_at = Some(now);
        self.stale_emitted = false;
        self.state.mirrored_data_stale = false;
    }

    pub fn poll_timeouts(&mut self, now: Instant) -> Option<SessionEvent> {
        if self.state.connection == ConnectionState::Identifying {
            if self.identify_deadline.is_some_and(|d| now >= d) {
                self.state.transition(ConnectionState::Disconnected);
                self.identify_deadline = None;
                return Some(SessionEvent::ConnectionChanged {
                    state: ConnectionState::Disconnected,
                    identity: None,
                    reason: Some("no id record within the connection timeout".into()),
                    reset_forced: false,
                });
            }
        }
        if let Some(flight) = &self.state.in_flight {
            if now >= flight.deadline {
                let command = flight.command.clone();
                self.state.in_flight = None;
                if self.state.connection == ConnectionState::Busy {
                    self.state.connection = ConnectionState::Ready;
                }
                if is_write_command(&command) {
                    self.write_unknown = true;
                    self.state.mirrored_data_stale = true;
                }
                if let Some(cal) = &mut self.calibration {
                    cal.interrupt();
                }
                return Some(SessionEvent::ProtocolError {
                    detail: "timeout: no acknowledgement inside the contract window".into(),
                });
            }
        }
        if self.telemetry_period_ms > 0 {
            if let Some(last) = self.last_telemetry_at {
                if now.duration_since(last) >= STALE_AFTER && !self.stale_emitted {
                    self.stale_emitted = true;
                    self.state.mirrored_data_stale = true;
                    return Some(SessionEvent::Staleness {
                        last_update_age_ms: now.duration_since(last).as_millis() as u64,
                    });
                }
            }
        }
        None
    }

    pub fn identity(&self) -> Option<&IdRecord> {
        self.state.identity.as_ref()
    }

    pub fn reset_forced(&self) -> bool {
        self.reset_forced
    }

    fn on_id(&mut self, id: IdRecord) -> Vec<SessionEvent> {
        if let Err(err) = validate_identity(&id) {
            self.state.transition(ConnectionState::Disconnected);
            self.identify_deadline = None;
            return vec![SessionEvent::ConnectionChanged {
                state: ConnectionState::Disconnected,
                identity: None,
                reason: Some(err.reason),
                reset_forced: false,
            }];
        }
        self.reset_forced = reset_forced_from_identity(&id);
        self.state.identity = Some(id.clone());
        self.identify_deadline = None;
        self.state.mirrored_data_stale = false;
        self.state.connection = ConnectionState::Ready;
        vec![SessionEvent::ConnectionChanged {
            state: ConnectionState::Ready,
            identity: Some(id),
            reason: None,
            reset_forced: self.reset_forced,
        }]
    }

    fn on_ack(&mut self, ack: AckRecord) -> Vec<SessionEvent> {
        self.last_ack = Some(ack.clone());
        let abort_ack = ack.command.eq_ignore_ascii_case("ABORT");
        if abort_ack {
            self.state.in_flight = None;
            if self.state.connection == ConnectionState::Busy {
                self.state.connection = ConnectionState::Ready;
            }
            return vec![SessionEvent::Telemetry {
                record: Record::Ack(ack),
            }];
        }
        if let Some(flight) = &self.state.in_flight {
            if flight.tag != ack.tag {
                return vec![SessionEvent::ProtocolError {
                    detail: format!(
                        "ack tag {} does not match in-flight tag {}",
                        ack.tag, flight.tag
                    ),
                }];
            }
        } else {
            return vec![SessionEvent::Telemetry {
                record: Record::Ack(ack),
            }];
        }
        self.state.in_flight = None;
        if self.state.connection == ConnectionState::Busy {
            self.state.connection = ConnectionState::Ready;
        }
        if !ack.ok {
            return vec![SessionEvent::Telemetry {
                record: Record::Ack(ack),
            }];
        }
        vec![SessionEvent::Telemetry {
            record: Record::Ack(ack),
        }]
    }

    fn update_mirror(&mut self, record: &Record) {
        match record {
            Record::Cal(cal) => {
                self.mirror.cal.retain(|c| c.motor != cal.motor);
                self.mirror.cal.push(cal.clone());
                self.mirror.cal.sort_by_key(|c| c.motor);
            }
            Record::Cfg(cfg) => self.mirror.cfg = Some(cfg.clone()),
            Record::Motor(motor) => {
                self.mirror.motors.retain(|m| m.motor != motor.motor);
                self.mirror.motors.push(motor.clone());
                self.mirror.motors.sort_by_key(|m| m.motor);
            }
            Record::Imp(imp) => {
                self.mirror.impedance.retain(|m| m.motor != imp.motor);
                self.mirror.impedance.push(imp.clone());
                self.mirror.impedance.sort_by_key(|m| m.motor);
            }
            Record::Timing(timing) => self.mirror.timing = Some(timing.clone()),
            Record::Bus(bus) => self.mirror.bus = Some(bus.clone()),
            _ => {}
        }
    }
}

pub fn validate_identity(id: &IdRecord) -> Result<(), SessionError> {
    if id.protocol_version != SUPPORTED_PROTOCOL_VERSION {
        return Err(SessionError::new(
            ErrorKind::Protocol,
            format!(
                "unsupported protocol version {} (supported {SUPPORTED_PROTOCOL_VERSION})",
                id.protocol_version
            ),
        ));
    }
    if !SUPPORTED_CONFIG_VERSIONS.contains(&id.config_version) {
        return Err(SessionError::new(
            ErrorKind::Protocol,
            format!("unsupported config version {}", id.config_version),
        ));
    }
    if id.motor_count != EXPECTED_MOTOR_COUNT {
        return Err(SessionError::new(
            ErrorKind::Protocol,
            format!(
                "unrecognised device: motor count {} (expected {EXPECTED_MOTOR_COUNT})",
                id.motor_count
            ),
        ));
    }
    Ok(())
}

pub fn timeout_for(command: &str) -> Duration {
    match command {
        "C1" | "C2" | "CA" => Duration::from_millis(CALIBRATION_TIMEOUT_MS),
        _ => Duration::from_millis(NORMAL_TIMEOUT_MS),
    }
}

pub fn applied_telemetry_period(requested_ms: u32) -> u32 {
    if requested_ms == 0 {
        0
    } else if requested_ms < QP_MIN_PERIOD_MS {
        QP_MIN_PERIOD_MS
    } else {
        requested_ms
    }
}

pub fn is_write_command(command: &str) -> bool {
    command.starts_with('N')
        || command.starts_with('B')
        || command.starts_with('V')
        || command.starts_with('M')
        || command == "CY"
}

#[cfg(test)]
mod tests {
    use super::*;

    fn id_line() -> &'static str {
        "#V13 v=1 t=id fw=002 proto=1 canid=0x202 motors=2 cfgver=2 uptime_ms=60000"
    }

    #[test]
    fn identify_reaches_ready() {
        let mut s = Session::default();
        let t0 = Instant::now();
        s.begin_connect("COM6".into(), t0);
        let events = s.ingest_line(id_line(), t0);
        assert_eq!(s.state.connection, ConnectionState::Ready);
        assert!(!s.reset_forced());
        assert!(matches!(
            events.last(),
            Some(SessionEvent::ConnectionChanged {
                state: ConnectionState::Ready,
                ..
            })
        ));
    }

    #[test]
    fn unsupported_protocol_is_refused() {
        let mut s = Session::default();
        let t0 = Instant::now();
        s.begin_connect("COM6".into(), t0);
        s.ingest_line(
            "#V13 v=1 t=id fw=002 proto=99 canid=0x202 motors=2 cfgver=2 uptime_ms=60000",
            t0,
        );
        assert_eq!(s.state.connection, ConnectionState::Disconnected);
        assert!(s.identity().is_none());
    }

    #[test]
    fn identify_timeout_is_unrecognised() {
        let mut s = Session::default();
        let t0 = Instant::now();
        s.begin_connect("COM6".into(), t0);
        let event = s.poll_timeouts(t0 + IDENTIFY_TIMEOUT);
        assert!(matches!(
            event,
            Some(SessionEvent::ConnectionChanged {
                state: ConnectionState::Disconnected,
                ..
            })
        ));
    }

    #[test]
    fn single_in_flight_and_tag_match() {
        let mut s = Session::default();
        let t0 = Instant::now();
        s.begin_connect("COM6".into(), t0);
        s.ingest_line(id_line(), t0);
        let bytes = s.prepare_request("Q", t0).unwrap();
        assert!(String::from_utf8_lossy(&bytes).starts_with("#1;Q"));
        assert_eq!(s.state.connection, ConnectionState::Busy);
        assert!(s.prepare_request("QC", t0).is_err());
        s.ingest_line("#V13 v=1 t=ack tag=1 cmd=Q ok=1 reason=", t0);
        assert_eq!(s.state.connection, ConnectionState::Ready);
        assert!(s.state.in_flight.is_none());
    }

    #[test]
    fn abort_is_permitted_while_busy() {
        let mut s = Session::default();
        let t0 = Instant::now();
        s.begin_connect("COM6".into(), t0);
        s.ingest_line(id_line(), t0);
        s.prepare_request("C1", t0).unwrap();
        assert_eq!(s.state.connection, ConnectionState::Busy);
        assert_eq!(s.prepare_abort().unwrap(), [0x18]);
        s.ingest_line("#V13 v=1 t=ack tag=0 cmd=ABORT ok=1 reason=", t0);
        assert_eq!(s.state.connection, ConnectionState::Ready);
        assert!(s.state.in_flight.is_none());
    }

    #[test]
    fn calibration_uses_long_timeout() {
        assert_eq!(
            timeout_for("C1"),
            Duration::from_millis(CALIBRATION_TIMEOUT_MS)
        );
        assert_eq!(timeout_for("Q"), Duration::from_millis(NORMAL_TIMEOUT_MS));
    }

    #[test]
    fn request_timeout_clears_busy_without_a_decision() {
        let mut s = Session::default();
        let t0 = Instant::now();
        s.begin_connect("COM6".into(), t0);
        s.ingest_line(id_line(), t0);
        s.prepare_request("N202", t0).unwrap();
        let event = s.poll_timeouts(t0 + Duration::from_millis(NORMAL_TIMEOUT_MS));
        assert!(matches!(event, Some(SessionEvent::ProtocolError { .. })));
        assert_eq!(s.state.connection, ConnectionState::Ready);
        assert!(s.state.in_flight.is_none());
        assert!(s.write_unknown);
    }

    #[test]
    fn telemetry_period_is_floored_to_firmware_minimum() {
        assert_eq!(applied_telemetry_period(0), 0);
        assert_eq!(applied_telemetry_period(10), QP_MIN_PERIOD_MS);
        assert_eq!(applied_telemetry_period(200), 200);
    }

    #[test]
    fn telemetry_is_stale_two_seconds_after_the_last_record() {
        let mut s = Session::default();
        let t0 = Instant::now();
        s.begin_connect("COM6".into(), t0);
        s.ingest_line(id_line(), t0);
        s.telemetry_period_ms = 200;
        s.note_telemetry(t0);
        assert!(s.poll_timeouts(t0 + Duration::from_millis(1999)).is_none());
        assert!(matches!(
            s.poll_timeouts(t0 + STALE_AFTER),
            Some(SessionEvent::Staleness { .. })
        ));
    }
}
