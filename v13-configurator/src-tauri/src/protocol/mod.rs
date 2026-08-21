//! Wire protocol: typed records and the parser that produces them.
//!
//! Contract: specs/003-v13-configurator/contracts/serial-protocol.md

pub mod encode;
pub mod parser;
pub mod records;

pub use encode::{encode_abort, encode_command, EncodeError};
pub use parser::{parse_line, ParseError, ParseOutcome};
pub use records::*;
