use crate::protocol::IdRecord;
use serde::Serialize;
use std::time::Instant;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum ConnectionState {
    Disconnected,
    Identifying,
    Ready,
    Busy,
    Lost,
}

#[derive(Debug, Clone)]
pub struct InFlight {
    pub tag: u8,
    pub command: String,
    pub deadline: Instant,
}

#[derive(Debug, Clone)]
pub struct SessionState {
    pub connection: ConnectionState,
    pub port_name: Option<String>,
    pub identity: Option<IdRecord>,
    pub in_flight: Option<InFlight>,
    pub next_tag: u8,
    pub intent_generation: u64,
    pub mirrored_data_stale: bool,
}

impl Default for SessionState {
    fn default() -> Self {
        Self {
            connection: ConnectionState::Disconnected,
            port_name: None,
            identity: None,
            in_flight: None,
            next_tag: 1,
            intent_generation: 0,
            mirrored_data_stale: true,
        }
    }
}

impl SessionState {
    pub fn transition(&mut self, next: ConnectionState) {
        if self.connection == ConnectionState::Ready && next != ConnectionState::Ready {
            self.in_flight = None;
            self.intent_generation = self.intent_generation.wrapping_add(1);
        }
        if next == ConnectionState::Lost {
            self.mirrored_data_stale = true;
        }
        if next == ConnectionState::Disconnected {
            self.port_name = None;
            self.identity = None;
        }
        self.connection = next;
    }

    pub fn allocate_tag(&mut self) -> u8 {
        let tag = if self.next_tag == 0 { 1 } else { self.next_tag };
        self.next_tag = tag.wrapping_add(1);
        if self.next_tag == 0 {
            self.next_tag = 1;
        }
        tag
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn leaving_ready_clears_request_and_invalidates_intent() {
        let mut s = SessionState::default();
        s.connection = ConnectionState::Ready;
        s.intent_generation = 4;
        s.in_flight = Some(InFlight {
            tag: 2,
            command: "N202".into(),
            deadline: Instant::now(),
        });
        s.transition(ConnectionState::Lost);
        assert!(s.in_flight.is_none());
        assert_eq!(s.intent_generation, 5);
    }
    #[test]
    fn lost_keeps_identity_but_marks_mirror_stale() {
        let mut s = SessionState::default();
        s.connection = ConnectionState::Ready;
        s.mirrored_data_stale = false;
        s.identity = Some(IdRecord {
            firmware_level: "002".into(),
            protocol_version: 1,
            can_id: 0x202,
            motor_count: 2,
            config_version: 2,
            uptime_ms: 1,
        });
        s.transition(ConnectionState::Lost);
        assert!(s.identity.is_some());
        assert!(s.mirrored_data_stale);
    }
    #[test]
    fn tags_never_allocate_zero() {
        let mut s = SessionState::default();
        s.next_tag = 255;
        assert_eq!(s.allocate_tag(), 255);
        assert_eq!(s.allocate_tag(), 1);
    }
}
