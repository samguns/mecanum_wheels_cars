use super::{ErrorKind, SessionError};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CalibrationPhase {
    Idle,
    Confirming,
    Running,
    Pending,
    Resolved,
    Failed,
    Unknown,
}

#[derive(Debug, Clone)]
pub struct CalibrationSession {
    pub motor: u8,
    pub phase: CalibrationPhase,
    pub pending_stored: bool,
    pub other_motor_energised: bool,
}

impl CalibrationSession {
    pub fn new(motor: u8) -> Self {
        Self {
            motor,
            phase: CalibrationPhase::Confirming,
            pending_stored: false,
            other_motor_energised: false,
        }
    }

    pub fn start(&mut self) -> Result<(), SessionError> {
        if self.phase != CalibrationPhase::Confirming {
            return Err(SessionError::new(ErrorKind::Busy, "not confirming"));
        }
        self.phase = CalibrationPhase::Running;
        self.pending_stored = false;
        Ok(())
    }

    pub fn stage_complete(&mut self) {
        if self.phase == CalibrationPhase::Running {
            self.phase = CalibrationPhase::Pending;
            self.pending_stored = false;
        }
    }

    pub fn accept_ack(&mut self, ok: bool) -> Result<(), SessionError> {
        if self.phase != CalibrationPhase::Pending {
            return Err(SessionError::new(ErrorKind::Protocol, "no pending result"));
        }
        if !ok {
            self.phase = CalibrationPhase::Failed;
            self.pending_stored = false;
            return Err(SessionError::new(ErrorKind::Refused, "accept was refused"));
        }
        self.pending_stored = true;
        self.phase = CalibrationPhase::Resolved;
        Ok(())
    }

    pub fn reject_ack(&mut self, ok: bool) {
        self.pending_stored = false;
        self.phase = if ok { CalibrationPhase::Idle } else { CalibrationPhase::Unknown };
    }

    pub fn interrupt(&mut self) {
        self.pending_stored = false;
        self.phase = CalibrationPhase::Unknown;
    }

    pub fn assert_unselected_idle(&self) -> Result<(), SessionError> {
        if self.other_motor_energised {
            return Err(SessionError::new(
                ErrorKind::Protocol,
                "unselected motor must stay de-energised",
            ));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pending_is_not_stored_without_successful_accept() {
        let mut s = CalibrationSession::new(1);
        s.start().unwrap();
        s.stage_complete();
        assert!(!s.pending_stored);
        assert!(s.accept_ack(false).is_err());
        assert!(!s.pending_stored);
        let mut s = CalibrationSession::new(1);
        s.start().unwrap();
        s.stage_complete();
        s.accept_ack(true).unwrap();
        assert!(s.pending_stored);
        assert_eq!(s.phase, CalibrationPhase::Resolved);
    }

    #[test]
    fn interrupt_is_unknown_not_failed() {
        let mut s = CalibrationSession::new(2);
        s.start().unwrap();
        s.interrupt();
        assert_eq!(s.phase, CalibrationPhase::Unknown);
        assert!(!s.pending_stored);
    }

    #[test]
    fn unselected_motor_must_not_be_in_powered_request() {
        let mut s = CalibrationSession::new(1);
        s.other_motor_energised = true;
        assert!(s.assert_unselected_idle().is_err());
    }
}
