//! Local operator profile and app preferences. Attribution only — no credentials.

use crate::session::{ErrorKind, SessionError};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct OperatorProfile {
    pub display_name: String,
    pub email: Option<String>,
}

impl Default for OperatorProfile {
    fn default() -> Self {
        Self {
            display_name: "anonymous".into(),
            email: None,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppSettings {
    pub last_port: Option<String>,
    pub telemetry_period_ms: u16,
    pub operator: OperatorProfile,
}

impl Default for AppSettings {
    fn default() -> Self {
        Self {
            last_port: None,
            telemetry_period_ms: 200,
            operator: OperatorProfile::default(),
        }
    }
}

pub fn data_dir() -> PathBuf {
    let base = std::env::var_os("APPDATA")
        .or_else(|| std::env::var_os("XDG_CONFIG_HOME"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("v13-configurator")
}

fn settings_path() -> PathBuf {
    data_dir().join("settings.json")
}

pub fn load() -> AppSettings {
    let path = settings_path();
    fs::read_to_string(path)
        .ok()
        .and_then(|raw| serde_json::from_str(&raw).ok())
        .unwrap_or_default()
}

pub fn save(settings: &AppSettings) -> Result<(), SessionError> {
    let dir = data_dir();
    fs::create_dir_all(&dir).map_err(|e| {
        SessionError::new(ErrorKind::Protocol, format!("settings dir: {e}"))
    })?;
    let raw = serde_json::to_string_pretty(settings)
        .map_err(|e| SessionError::new(ErrorKind::Protocol, e.to_string()))?;
    fs::write(settings_path(), raw)
        .map_err(|e| SessionError::new(ErrorKind::Protocol, format!("settings write: {e}")))
}

pub fn operator_label(settings: &AppSettings) -> String {
    if settings.operator.display_name.trim().is_empty() {
        "anonymous".into()
    } else {
        settings.operator.display_name.clone()
    }
}
