use std::fmt;

pub const ABORT_BYTE: u8 = 0x18;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EncodeError {
    ReservedTag,
    Empty,
    ContainsDelimiter,
}

impl fmt::Display for EncodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::ReservedTag => "tag 0 is reserved",
            Self::Empty => "command is empty",
            Self::ContainsDelimiter => "command contains a line delimiter",
        })
    }
}

pub fn encode_command(tag: u8, command: &str) -> Result<Vec<u8>, EncodeError> {
    if tag == 0 {
        return Err(EncodeError::ReservedTag);
    }
    if command.is_empty() {
        return Err(EncodeError::Empty);
    }
    if command.contains(['\r', '\n', ';']) {
        return Err(EncodeError::ContainsDelimiter);
    }
    Ok(format!("#{tag};{command}\n").into_bytes())
}

pub fn encode_abort() -> [u8; 1] {
    [ABORT_BYTE]
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tagged_command_is_framed() {
        assert_eq!(encode_command(7, "CY").unwrap(), b"#7;CY\n");
    }
    #[test]
    fn abort_is_raw_and_unframed() {
        assert_eq!(encode_abort(), [0x18]);
    }
    #[test]
    fn unsafe_inputs_are_rejected() {
        assert!(encode_command(0, "Q").is_err());
        assert!(encode_command(1, "Q\nD0").is_err());
    }
}
