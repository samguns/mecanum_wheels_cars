use super::{ErrorKind, SessionError};
use std::time::{Duration, Instant};

pub const INTENT_TTL: Duration = Duration::from_secs(30);

#[derive(Debug, Clone)]
pub struct IntentToken {
    pub motor: u8,
    pub wheel_name: String,
    pub precondition: String,
    pub expires_at: Instant,
    pub generation: u64,
    used: bool,
}

impl IntentToken {
    pub fn issue(motor: u8, wheel_name: String, precondition: String, generation: u64, now: Instant) -> Self {
        Self {
            motor,
            wheel_name,
            precondition,
            expires_at: now + INTENT_TTL,
            generation,
            used: false,
        }
    }

    pub fn consume(&mut self, motor: u8, generation: u64, now: Instant) -> Result<(), SessionError> {
        if self.used {
            return Err(SessionError::new(
                ErrorKind::NeedsConfirmation,
                "intent token already used",
            ));
        }
        if now >= self.expires_at {
            return Err(SessionError::new(ErrorKind::NeedsConfirmation, "intent token expired"));
        }
        if self.motor != motor {
            return Err(SessionError::new(
                ErrorKind::NeedsConfirmation,
                format!("intent was issued for motor {} not {motor}", self.motor),
            ));
        }
        if self.generation != generation {
            return Err(SessionError::new(
                ErrorKind::NeedsConfirmation,
                "intent was invalidated when the session left ready",
            ));
        }
        self.used = true;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn single_use_and_wrong_motor_and_expiry() {
        let t0 = Instant::now();
        let mut token = IntentToken::issue(1, "Rear Right".into(), "wheels clear".into(), 1, t0);
        assert!(token.consume(2, 1, t0).is_err());
        assert!(token.consume(1, 1, t0).is_ok());
        assert!(token.consume(1, 1, t0).is_err());
        let mut expired = IntentToken::issue(1, "Rear Right".into(), "wheels clear".into(), 1, t0);
        assert!(expired.consume(1, 1, t0 + INTENT_TTL).is_err());
    }
}
