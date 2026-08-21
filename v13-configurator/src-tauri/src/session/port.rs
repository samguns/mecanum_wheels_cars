//! Exclusive serial ownership and the ESP32 non-resetting open sequence.
//!
//! On most ESP32 USB-serial bridges DTR and RTS are wired to EN and IO0, so the default
//! asserted signals reboot the controller. This module always deasserts both after open.
//! Whether that fully suppresses reset is board-dependent (research M1); a forced reset is
//! reported from the first `id` record's uptime rather than hidden.

use super::{ErrorKind, SessionError};
use crate::protocol::IdRecord;
use serde::Serialize;
use serialport::{DataBits, FlowControl, Parity, SerialPort, StopBits};
use std::time::Duration;

pub const BAUD: u32 = 115_200;
pub const READ_TIMEOUT: Duration = Duration::from_millis(50);
/// DTR deasserted: the non-resetting state on typical ESP32 USB-serial bridges.
pub const NON_RESETTING_DTR: bool = false;
/// RTS deasserted: the non-resetting state on typical ESP32 USB-serial bridges.
pub const NON_RESETTING_RTS: bool = false;
/// An `id` uptime below the identification window means the board almost certainly rebooted on open.
pub const RESET_UPTIME_THRESHOLD_MS: u32 = 2_000;

#[derive(Debug, Clone, Serialize)]
pub struct PortDescriptor {
    pub name: String,
    pub port_type: String,
}

pub fn list_ports() -> Result<Vec<PortDescriptor>, SessionError> {
    let mut ports = serialport::available_ports().map_err(|e| {
        SessionError::new(ErrorKind::PortLost, format!("port enumeration failed: {e}"))
    })?;
    ports.sort_by(|a, b| a.port_name.cmp(&b.port_name));
    Ok(ports
        .into_iter()
        .map(|p| PortDescriptor {
            name: p.port_name,
            port_type: format!("{:?}", p.port_type),
        })
        .collect())
}

/// Opens `name` exclusively at 115200 8N1 and forces DTR/RTS to the non-resetting state.
pub fn open_exclusive(name: &str) -> Result<Box<dyn SerialPort>, SessionError> {
    let mut port = serialport::new(name, BAUD)
        .timeout(READ_TIMEOUT)
        .data_bits(DataBits::Eight)
        .parity(Parity::None)
        .stop_bits(StopBits::One)
        .flow_control(FlowControl::None)
        .open()
        .map_err(|e| SessionError::new(ErrorKind::PortLost, format!("open {name} failed: {e}")))?;
    apply_non_resetting_signals(port.as_mut())?;
    Ok(port)
}

pub fn apply_non_resetting_signals(port: &mut dyn SerialPort) -> Result<(), SessionError> {
    port.write_data_terminal_ready(NON_RESETTING_DTR)
        .map_err(|e| SessionError::new(ErrorKind::PortLost, format!("DTR: {e}")))?;
    port.write_request_to_send(NON_RESETTING_RTS)
        .map_err(|e| SessionError::new(ErrorKind::PortLost, format!("RTS: {e}")))?;
    Ok(())
}

/// `true` when the first identity record shows a boot-fresh uptime, i.e. the board reset on open.
pub fn reset_forced_from_identity(id: &IdRecord) -> bool {
    id.uptime_ms < RESET_UPTIME_THRESHOLD_MS
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn non_resetting_signals_are_deasserted() {
        assert!(!NON_RESETTING_DTR);
        assert!(!NON_RESETTING_RTS);
    }

    #[test]
    fn boot_uptime_is_reported_as_forced_reset() {
        let id = IdRecord {
            firmware_level: "002".into(),
            protocol_version: 1,
            can_id: 0x202,
            motor_count: 2,
            config_version: 2,
            uptime_ms: 80,
        };
        assert!(reset_forced_from_identity(&id));
    }

    #[test]
    fn long_uptime_is_not_a_reset() {
        let id = IdRecord {
            firmware_level: "002".into(),
            protocol_version: 1,
            can_id: 0x202,
            motor_count: 2,
            config_version: 2,
            uptime_ms: 60_000,
        };
        assert!(!reset_forced_from_identity(&id));
    }
}
