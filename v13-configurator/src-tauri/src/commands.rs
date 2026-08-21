//! Tauri command and event surface. Contract: `contracts/app-ipc.md`.
//!
//! The frontend never opens a port or parses a serial line. Every command goes through the
//! exclusive session in this module.

use crate::protocol::{AckRecord, IdRecord};
use crate::session::port::{list_ports as enumerate_ports, open_exclusive, PortDescriptor};
use crate::session::{
    applied_telemetry_period, timeout_for, wheel_label, CalibrationSession, ConnectResult,
    ConnectionState, DeviceMirror, ErrorKind, IntentToken, Session, SessionError, SessionEvent,
};
use crate::storage;
use serde::{Deserialize, Serialize};
use std::io::{Read, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager, State};

pub struct AppState {
    pub live: Mutex<Live>,
}

pub struct Live {
    pub session: Session,
    pub port: Option<Box<dyn serialport::SerialPort + Send>>,
    pub stop_reader: Arc<AtomicBool>,
    pub selected_motor: u8,
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            live: Mutex::new(Live {
                session: Session::default(),
                port: None,
                stop_reader: Arc::new(AtomicBool::new(false)),
                selected_motor: 1,
            }),
        }
    }
}

fn emit_events(app: &AppHandle, events: Vec<SessionEvent>) {
    for event in events {
        match &event {
            SessionEvent::ConnectionChanged { .. } => {
                let _ = app.emit("connection_changed", &event);
            }
            SessionEvent::Telemetry { .. } => {
                let _ = app.emit("telemetry", &event);
            }
            SessionEvent::CalibrationProgress { .. } => {
                let _ = app.emit("calibration_progress", &event);
            }
            SessionEvent::CalibrationPending { .. } => {
                let _ = app.emit("calibration_pending", &event);
            }
            SessionEvent::Fault { .. } => {
                let _ = app.emit("fault", &event);
            }
            SessionEvent::ProtocolError { .. } => {
                let _ = app.emit("protocol_error", &event);
            }
            SessionEvent::Staleness { .. } => {
                let _ = app.emit("staleness", &event);
            }
        }
    }
}

fn spawn_reader(app: AppHandle, stop: Arc<AtomicBool>) {
    thread::spawn(move || {
        let mut buf = [0u8; 512];
        while !stop.load(Ordering::SeqCst) {
            let n = {
                let Some(managed) = app.try_state::<AppState>() else {
                    break;
                };
                let mut live = match managed.live.lock() {
                    Ok(g) => g,
                    Err(_) => break,
                };
                match live.port.as_mut() {
                    Some(port) => match port.read(&mut buf) {
                        Ok(n) => n,
                        Err(err)
                            if err.kind() == std::io::ErrorKind::TimedOut
                                || err.kind() == std::io::ErrorKind::WouldBlock =>
                        {
                            let now = Instant::now();
                            if let Some(event) = live.session.poll_timeouts(now) {
                                drop(live);
                                emit_events(&app, vec![event]);
                            }
                            continue;
                        }
                        Err(_) => {
                            live.session.mark_lost("port lost");
                            live.port = None;
                            drop(live);
                            emit_events(
                                &app,
                                vec![SessionEvent::ConnectionChanged {
                                    state: ConnectionState::Lost,
                                    identity: None,
                                    reason: Some("port lost".into()),
                                    reset_forced: false,
                                }],
                            );
                            break;
                        }
                    },
                    None => break,
                }
            };
            if n == 0 {
                continue;
            }
            let Some(managed) = app.try_state::<AppState>() else {
                break;
            };
            let mut live = match managed.live.lock() {
                Ok(g) => g,
                Err(_) => break,
            };
            let events = live.session.ingest_bytes(&buf[..n], Instant::now());
            drop(live);
            emit_events(&app, events);
        }
    });
}

#[tauri::command]
pub fn list_ports() -> Result<Vec<PortDescriptor>, SessionError> {
    enumerate_ports()
}

#[derive(Debug, Deserialize)]
pub struct ConnectArgs {
    pub port: String,
}

#[tauri::command]
pub fn connect(
    app: AppHandle,
    state: State<AppState>,
    args: ConnectArgs,
) -> Result<ConnectResult, SessionError> {
    disconnect_inner(&state)?;
    let mut port = open_exclusive(&args.port)?;
    let mut live = state.live.lock().map_err(|_| {
        SessionError::new(ErrorKind::PortLost, "session lock poisoned")
    })?;
    live.session.begin_connect(args.port.clone(), Instant::now());
    // Firmware emits an unsolicited id only at boot. A later attach must ask.
    // `Q` is the query that includes `t=id`. `QI` is motors, not identity.
    match crate::protocol::encode_command(1, "Q") {
        Ok(probe) => {
            if let Err(err) = port.write_all(&probe) {
                live.session.disconnect();
                return Err(SessionError::new(
                    ErrorKind::PortLost,
                    format!("identify probe failed: {err}"),
                ));
            }
        }
        Err(err) => {
            live.session.disconnect();
            return Err(SessionError::new(ErrorKind::Protocol, err.to_string()));
        }
    }
    let deadline = Instant::now() + crate::session::IDENTIFY_TIMEOUT;
    let mut buf = [0u8; 512];
    while Instant::now() < deadline && live.session.state.connection == ConnectionState::Identifying
    {
        match port.read(&mut buf) {
            Ok(0) => {}
            Ok(n) => {
                let events = live.session.ingest_bytes(&buf[..n], Instant::now());
                emit_events(&app, events);
            }
            Err(err)
                if err.kind() == std::io::ErrorKind::TimedOut
                    || err.kind() == std::io::ErrorKind::WouldBlock => {}
            Err(err) => {
                return Err(SessionError::new(
                    ErrorKind::PortLost,
                    format!("read during identify: {err}"),
                ));
            }
        }
        if let Some(event) = live.session.poll_timeouts(Instant::now()) {
            emit_events(&app, vec![event]);
        }
    }
    if live.session.state.connection != ConnectionState::Ready {
        live.session.disconnect();
        return Err(SessionError::new(
            ErrorKind::Protocol,
            "unrecognised device or no id record within the connection timeout",
        ));
    }
    let result = ConnectResult {
        identity: live.session.identity().cloned().ok_or_else(|| {
            SessionError::new(ErrorKind::Protocol, "identified without an id record")
        })?,
        reset_forced: live.session.reset_forced(),
    };
    live.stop_reader = Arc::new(AtomicBool::new(false));
    let stop = live.stop_reader.clone();
    live.port = Some(port);
    drop(live);
    spawn_reader(app, stop);
    Ok(result)
}

fn disconnect_inner(state: &State<AppState>) -> Result<(), SessionError> {
    let mut live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    live.stop_reader.store(true, Ordering::SeqCst);
    live.port = None;
    live.session.disconnect();
    Ok(())
}

#[tauri::command]
pub fn disconnect(state: State<AppState>) -> Result<(), SessionError> {
    disconnect_inner(&state)
}

fn write_bytes(state: &AppState, bytes: &[u8]) -> Result<(), SessionError> {
    let mut live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    let port = live
        .port
        .as_mut()
        .ok_or_else(|| SessionError::new(ErrorKind::NotConnected, "no live session"))?;
    port.write_all(bytes)
        .map_err(|e| SessionError::new(ErrorKind::PortLost, format!("write failed: {e}")))?;
    Ok(())
}

fn invoke_and_wait(state: &AppState, command: &str) -> Result<AckRecord, SessionError> {
    let timeout = timeout_for(command);
    let bytes = {
        let mut live = state
            .live
            .lock()
            .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
        live.session.last_ack = None;
        live.session.prepare_request(command, Instant::now())?
    };
    write_bytes(state, &bytes)?;
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        thread::sleep(Duration::from_millis(10));
        let live = state
            .live
            .lock()
            .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
        if live.session.state.connection != ConnectionState::Busy {
            if let Some(ack) = &live.session.last_ack {
                if !ack.ok {
                    return Err(SessionError::new(ErrorKind::Refused, ack.reason.clone()));
                }
                return Ok(ack.clone());
            }
            return Err(SessionError::new(
                ErrorKind::Timeout,
                "request ended without an acknowledgement",
            ));
        }
    }
    Err(SessionError::new(
        ErrorKind::Timeout,
        "no acknowledgement inside the contract window",
    ))
}

#[tauri::command]
pub fn abort(state: State<AppState>) -> Result<AckRecord, SessionError> {
    let bytes = {
        let mut live = state
            .live
            .lock()
            .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
        live.session.last_ack = None;
        live.session.prepare_abort()?
    };
    write_bytes(&*state, &bytes)?;
    let deadline = Instant::now() + Duration::from_millis(crate::session::NORMAL_TIMEOUT_MS);
    while Instant::now() < deadline {
        thread::sleep(Duration::from_millis(10));
        let live = state
            .live
            .lock()
            .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
        if let Some(ack) = &live.session.last_ack {
            if ack.command.eq_ignore_ascii_case("ABORT") {
                return Ok(ack.clone());
            }
        }
    }
    Err(SessionError::new(
        ErrorKind::Timeout,
        "no abort acknowledgement",
    ))
}

#[derive(Debug, Clone, Serialize)]
pub struct ControllerSnapshot {
    pub identity: Option<IdRecord>,
    pub mirror: DeviceMirror,
}

#[tauri::command]
pub fn read_all(state: State<AppState>) -> Result<ControllerSnapshot, SessionError> {
    invoke_and_wait(&state, "Q")?;
    let live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    Ok(ControllerSnapshot {
        identity: live.session.identity().cloned(),
        mirror: live.session.mirror.clone(),
    })
}

#[derive(Debug, Deserialize)]
pub struct TelemetryArgs {
    pub period_ms: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct TelemetryResult {
    pub applied_period_ms: u32,
    pub ack: AckRecord,
}

#[tauri::command]
pub fn set_telemetry(state: State<AppState>, args: TelemetryArgs) -> Result<TelemetryResult, SessionError> {
    let ack = invoke_and_wait(&state, &format!("QP{}", args.period_ms))?;
    let applied = applied_telemetry_period(args.period_ms);
    let mut live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    live.session.telemetry_period_ms = applied;
    Ok(TelemetryResult {
        applied_period_ms: applied,
        ack,
    })
}

#[derive(Debug, Deserialize)]
pub struct SelectMotorArgs {
    pub motor: u8,
}

#[tauri::command]
pub fn select_motor(state: State<AppState>, args: SelectMotorArgs) -> Result<(), SessionError> {
    if args.motor != 1 && args.motor != 2 {
        return Err(SessionError::new(
            ErrorKind::Protocol,
            "motor must be 1 or 2",
        ));
    }
    let mut live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    live.selected_motor = args.motor;
    Ok(())
}

#[derive(Debug, Deserialize)]
pub struct MotorArgs {
    pub motor: u8,
}

#[tauri::command]
pub fn arm(state: State<AppState>, args: MotorArgs) -> Result<AckRecord, SessionError> {
    invoke_and_wait(&state, &format!("A{}", args.motor))
}

#[tauri::command]
pub fn disarm(state: State<AppState>, args: MotorArgs) -> Result<AckRecord, SessionError> {
    invoke_and_wait(&state, &format!("D{}", args.motor))
}

#[derive(Debug, Deserialize)]
pub struct ImpedanceApplyArgs {
    pub motor: u8,
    pub p_des_mrad: i32,
    pub v_des: f32,
    pub kp: f32,
    pub kd: f32,
    pub t_ff: f32,
}

const IMPEDANCE_P_MRAD_LIMIT: i32 = 628319;

#[tauri::command]
pub fn apply_impedance(
    state: State<AppState>,
    args: ImpedanceApplyArgs,
) -> Result<AckRecord, SessionError> {
    if args.motor != 1 && args.motor != 2 {
        return Err(SessionError::new(ErrorKind::Protocol, "motor must be 1 or 2"));
    }
    if args.p_des_mrad.abs() > IMPEDANCE_P_MRAD_LIMIT {
        return Err(SessionError::new(
            ErrorKind::Protocol,
            "p_des must be -628319 to 628319 mrad",
        ));
    }
    if !(args.v_des >= -45.0 && args.v_des <= 45.0) {
        return Err(SessionError::new(ErrorKind::Protocol, "v_des must be -45 to 45 rad/s"));
    }
    if !(args.kp >= 0.0 && args.kp <= 50.0) {
        return Err(SessionError::new(ErrorKind::Protocol, "kp must be 0 to 50"));
    }
    if !(args.kd >= 0.0 && args.kd <= 1.0) {
        return Err(SessionError::new(ErrorKind::Protocol, "kd must be 0 to 1"));
    }
    if !(args.t_ff >= -0.5 && args.t_ff <= 0.5) {
        return Err(SessionError::new(ErrorKind::Protocol, "t_ff must be -0.5 to 0.5 Nm"));
    }
    let command = format!(
        "K{},{},{:.3},{:.3},{:.3},{:.4}",
        args.motor, args.p_des_mrad, args.v_des, args.kp, args.kd, args.t_ff
    );
    invoke_and_wait(&state, &command)
}

#[tauri::command]
pub fn calibrate_start(state: State<AppState>) -> Result<AckRecord, SessionError> {
    let command = {
        let mut live = state
            .live
            .lock()
            .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
        let motor = live.selected_motor;
        let generation = live.session.state.intent_generation;
        let token = live.session.intent.as_mut().ok_or_else(|| {
            SessionError::new(
                ErrorKind::NeedsConfirmation,
                "calibrate_start requires a confirmed intent token",
            )
        })?;
        token.consume(motor, generation, Instant::now())?;
        live.session.intent = None;
        let mut cal = CalibrationSession::new(motor);
        cal.start()?;
        live.session.calibration = Some(cal);
        format!("C{motor}")
    };
    match invoke_and_wait(&state, &command) {
        Ok(_) => {}
        Err(err) => {
            if let Ok(mut live) = state.live.lock() {
                if let Some(cal) = &mut live.session.calibration {
                    cal.interrupt();
                }
            }
            return Err(err);
        }
    }
    match invoke_and_wait(&state, "CA") {
        Ok(ack) => Ok(ack),
        Err(err) => {
            if let Ok(mut live) = state.live.lock() {
                if let Some(cal) = &mut live.session.calibration {
                    cal.interrupt();
                }
            }
            Err(err)
        }
    }
}

#[tauri::command]
pub fn calibrate_accept(state: State<AppState>) -> Result<AckRecord, SessionError> {
    match invoke_and_wait(&state, "CY") {
        Ok(ack) => {
            if let Ok(mut live) = state.live.lock() {
                if let Some(cal) = &mut live.session.calibration {
                    let _ = cal.accept_ack(true);
                }
            }
            let before = snapshot_mirror(&state).ok().map(|s| s.mirror.cal);
            let _ = invoke_and_wait(&state, "Q");
            let after = snapshot_mirror(&state).ok().map(|s| s.mirror.cal);
            append_change(
                &state,
                storage::changelog::ChangeKind::CalibrationAccepted,
                before,
                after,
            )?;
            Ok(ack)
        }
        Err(err) => {
            if let Ok(mut live) = state.live.lock() {
                if let Some(cal) = &mut live.session.calibration {
                    if err.kind == ErrorKind::Refused {
                        let _ = cal.accept_ack(false);
                    } else {
                        cal.interrupt();
                    }
                }
            }
            Err(err)
        }
    }
}

#[tauri::command]
pub fn calibrate_reject(state: State<AppState>) -> Result<AckRecord, SessionError> {
    match invoke_and_wait(&state, "CN") {
        Ok(ack) => {
            if let Ok(mut live) = state.live.lock() {
                if let Some(cal) = &mut live.session.calibration {
                    cal.reject_ack(true);
                }
            }
            Ok(ack)
        }
        Err(err) => {
            if let Ok(mut live) = state.live.lock() {
                if let Some(cal) = &mut live.session.calibration {
                    cal.reject_ack(false);
                }
            }
            Err(err)
        }
    }
}

#[derive(Debug, Deserialize)]
pub struct WriteSettingArgs {
    pub command: String,
}

#[tauri::command]
pub fn write_setting(state: State<AppState>, args: WriteSettingArgs) -> Result<AckRecord, SessionError> {
    if !crate::session::is_write_command(&args.command)
        || args.command.starts_with("CY")
        || args.command.starts_with('a')
        || args.command.starts_with('b')
    {
        return Err(SessionError::new(
            ErrorKind::Protocol,
            "write_setting accepts only N, B, V, or M",
        ));
    }
    let before = snapshot_mirror(&state).ok().and_then(|s| s.mirror.cfg);
    match invoke_and_wait(&state, &args.command) {
        Ok(ack) => {
            if let Ok(mut live) = state.live.lock() {
                live.session.write_unknown = false;
            }
            let _ = invoke_and_wait(&state, "Q");
            let after = snapshot_mirror(&state).ok().and_then(|s| s.mirror.cfg);
            append_change(
                &state,
                storage::changelog::ChangeKind::SettingWritten,
                before,
                after,
            )?;
            Ok(ack)
        }
        Err(err) => {
            if err.kind != ErrorKind::Refused {
                if let Ok(mut live) = state.live.lock() {
                    live.session.write_unknown = true;
                    live.session.state.mirrored_data_stale = true;
                }
                return Err(SessionError::new(
                    err.kind,
                    format!(
                        "stored state unknown; re-read required ({})",
                        err.reason
                    ),
                ));
            }
            Err(err)
        }
    }
}

#[tauri::command]
pub fn change_log_read() -> Result<Vec<storage::changelog::ChangeLogEntry>, SessionError> {
    storage::changelog::read_all()
}

#[derive(Debug, Deserialize)]
pub struct ConfirmIntentArgs {
    pub motor: u8,
    pub wheel_name: String,
    pub precondition: String,
}

#[tauri::command]
pub fn confirm_intent(state: State<AppState>, args: ConfirmIntentArgs) -> Result<(), SessionError> {
    if args.motor != 1 && args.motor != 2 {
        return Err(SessionError::new(ErrorKind::Protocol, "motor must be 1 or 2"));
    }
    let mut live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    if live.session.state.connection != ConnectionState::Ready {
        return Err(SessionError::new(
            ErrorKind::NotConnected,
            "confirm intent only while ready",
        ));
    }
    live.selected_motor = args.motor;
    let generation = live.session.state.intent_generation;
    live.session.intent = Some(IntentToken::issue(
        args.motor,
        args.wheel_name,
        args.precondition,
        generation,
        Instant::now(),
    ));
    Ok(())
}

#[tauri::command]
pub fn operator_get() -> storage::settings::OperatorProfile {
    storage::settings::load().operator
}

#[derive(Debug, Deserialize)]
pub struct OperatorArgs {
    pub display_name: String,
    pub email: Option<String>,
}

#[tauri::command]
pub fn operator_set(args: OperatorArgs) -> Result<storage::settings::OperatorProfile, SessionError> {
    let mut settings = storage::settings::load();
    settings.operator.display_name = if args.display_name.trim().is_empty() {
        "anonymous".into()
    } else {
        args.display_name
    };
    settings.operator.email = args.email.filter(|e| !e.trim().is_empty());
    storage::settings::save(&settings)?;
    Ok(settings.operator)
}

fn snapshot_mirror(state: &AppState) -> Result<ControllerSnapshot, SessionError> {
    let live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    Ok(ControllerSnapshot {
        identity: live.session.identity().cloned(),
        mirror: live.session.mirror.clone(),
    })
}

fn append_change<T: serde::Serialize>(
    state: &AppState,
    kind: storage::changelog::ChangeKind,
    before: Option<T>,
    after: Option<T>,
) -> Result<(), SessionError> {
    let live = state
        .live
        .lock()
        .map_err(|_| SessionError::new(ErrorKind::PortLost, "session lock poisoned"))?;
    let identity = live.session.identity().cloned().ok_or_else(|| {
        SessionError::new(
            ErrorKind::NotConnected,
            "changelog requires an identified session",
        )
    })?;
    let motor = live.selected_motor;
    let before = serde_json::to_value(before)
        .map_err(|e| SessionError::new(ErrorKind::Protocol, e.to_string()))?;
    let after = serde_json::to_value(after)
        .map_err(|e| SessionError::new(ErrorKind::Protocol, e.to_string()))?;
    let entry = storage::changelog::ChangeLogEntry {
        timestamp: timestamp_now(),
        operator: storage::changelog::attributed_operator(),
        can_id: identity.can_id,
        motor_index: Some(motor),
        wheel_label: wheel_label(identity.can_id, motor),
        kind,
        before,
        after,
        firmware_level: identity.firmware_level,
        protocol_version: identity.protocol_version,
    };
    drop(live);
    storage::changelog::append(&entry)
}

fn timestamp_now() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    format!("{}", now.as_secs())
}
