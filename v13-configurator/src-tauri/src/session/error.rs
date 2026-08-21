use serde::Serialize;
use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ErrorKind {
    NotConnected,
    Refused,
    Timeout,
    Protocol,
    PortLost,
    Busy,
    NeedsConfirmation,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct SessionError {
    pub kind: ErrorKind,
    pub reason: String,
}

impl SessionError {
    pub fn new(kind: ErrorKind, reason: impl Into<String>) -> Self {
        Self {
            kind,
            reason: reason.into(),
        }
    }
}
impl fmt::Display for SessionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.reason)
    }
}
impl std::error::Error for SessionError {}
