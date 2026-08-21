//! Append-only local change log. Before/after values come from device reads.

use super::settings::{data_dir, operator_label};
use crate::session::{ErrorKind, SessionError};
use serde::{Deserialize, Serialize};
use std::fs::{self, OpenOptions};
use std::io::Write;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ChangeKind {
    CalibrationAccepted,
    SettingWritten,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChangeLogEntry {
    pub timestamp: String,
    pub operator: String,
    pub can_id: u16,
    pub motor_index: Option<u8>,
    pub wheel_label: String,
    pub kind: ChangeKind,
    pub before: serde_json::Value,
    pub after: serde_json::Value,
    pub firmware_level: String,
    pub protocol_version: u16,
}

fn log_path() -> std::path::PathBuf {
    data_dir().join("changelog.jsonl")
}

pub fn append(entry: &ChangeLogEntry) -> Result<(), SessionError> {
    let dir = data_dir();
    fs::create_dir_all(&dir).map_err(|e| {
        SessionError::new(ErrorKind::Protocol, format!("changelog dir: {e}"))
    })?;
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path())
        .map_err(|e| SessionError::new(ErrorKind::Protocol, format!("changelog open: {e}")))?;
    let line = serde_json::to_string(entry)
        .map_err(|e| SessionError::new(ErrorKind::Protocol, e.to_string()))?;
    writeln!(file, "{line}")
        .map_err(|e| SessionError::new(ErrorKind::Protocol, format!("changelog write: {e}")))
}

pub fn read_all() -> Result<Vec<ChangeLogEntry>, SessionError> {
    let raw = match fs::read_to_string(log_path()) {
        Ok(s) => s,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(err) => {
            return Err(SessionError::new(
                ErrorKind::Protocol,
                format!("changelog read: {err}"),
            ))
        }
    };
    let mut entries = Vec::new();
    for line in raw.lines() {
        if line.trim().is_empty() {
            continue;
        }
        let entry: ChangeLogEntry = serde_json::from_str(line).map_err(|e| {
            SessionError::new(ErrorKind::Protocol, format!("changelog parse: {e}"))
        })?;
        entries.push(entry);
    }
    Ok(entries)
}

pub fn attributed_operator() -> String {
    operator_label(&super::settings::load())
}

#[cfg(test)]
mod tests {
    #[test]
    fn empty_name_is_explicitly_anonymous() {
        let settings = crate::storage::settings::AppSettings {
            operator: crate::storage::settings::OperatorProfile {
                display_name: "  ".into(),
                email: None,
            },
            ..Default::default()
        };
        assert_eq!(crate::storage::settings::operator_label(&settings), "anonymous");
    }
}
