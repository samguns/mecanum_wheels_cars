//! Pure record parser. No I/O, no globals, no allocation beyond the returned record.
//!
//! This is the single highest-consequence function in the feature: a bug here silently misreports a
//! motor's electrical parameters. It is therefore kept pure and exhaustively tested against the
//! shared fixture corpus, including the real firmware prose that must be ignored.
//!
//! Contract: specs/003-v13-configurator/contracts/serial-protocol.md

use super::records::*;

/// The protocol versions this build understands. A record carrying anything else is refused rather
/// than guessed at (FR-004).
pub const SUPPORTED_VERSIONS: &[u16] = &[1];

const PREFIX: &str = "#V13";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ParseError {
    /// `v=` was absent or was not the first field.
    MissingVersion,
    /// `t=` was absent or was not the second field.
    MissingType,
    UnsupportedVersion(String),
    UnknownType(String),
    /// A token had no `=`, or an empty key.
    MalformedField(String),
    /// The same key appeared twice; the intended value would be ambiguous.
    DuplicateField(String),
    /// A required key was absent for this record type.
    MissingField(&'static str),
    BadValue {
        key: String,
        value: String,
    },
}

#[derive(Debug, Clone, PartialEq)]
pub enum ParseOutcome {
    /// Not a structured record. Human prose and anything else on the wire lands here and is inert.
    Ignored,
    Record(Record),
    Error(ParseError),
}

/// Parses one line. Never panics on any input.
pub fn parse_line(line: &str) -> ParseOutcome {
    let trimmed = line.trim_end_matches(['\r', '\n']).trim();

    // Anything not carrying the prefix is inert. This is what lets a human share the session: the
    // firmware's own prose can never be mistaken for a record.
    if trimmed != PREFIX && !trimmed.starts_with(&format!("{PREFIX} ")) {
        return ParseOutcome::Ignored;
    }

    let rest = trimmed[PREFIX.len()..].trim_start();
    let mut fields: Vec<(&str, &str)> = Vec::new();

    for token in rest.split_whitespace() {
        let Some((key, value)) = token.split_once('=') else {
            return ParseOutcome::Error(ParseError::MalformedField(token.to_string()));
        };
        if key.is_empty() {
            return ParseOutcome::Error(ParseError::MalformedField(token.to_string()));
        }
        if fields.iter().any(|(k, _)| *k == key) {
            return ParseOutcome::Error(ParseError::DuplicateField(key.to_string()));
        }
        fields.push((key, value));
    }

    // `v` must be first and `t` second. Positional discipline means a reader can reject an
    // unsupported version before interpreting anything else.
    match fields.first() {
        Some(("v", raw)) => {
            let Ok(version) = raw.parse::<u16>() else {
                return ParseOutcome::Error(ParseError::UnsupportedVersion(raw.to_string()));
            };
            if !SUPPORTED_VERSIONS.contains(&version) {
                return ParseOutcome::Error(ParseError::UnsupportedVersion(raw.to_string()));
            }
        }
        _ => return ParseOutcome::Error(ParseError::MissingVersion),
    }

    let record_type = match fields.get(1) {
        Some(("t", raw)) if !raw.is_empty() => *raw,
        _ => return ParseOutcome::Error(ParseError::MissingType),
    };

    let f = Fields { fields: &fields };

    let parsed = match record_type {
        "id" => parse_id(&f),
        "cal" => parse_cal(&f),
        "cfg" => parse_cfg(&f),
        "motor" => parse_motor(&f),
        "imp" => parse_imp(&f),
        "timing" => parse_timing(&f),
        "bus" => parse_bus(&f),
        "ack" => parse_ack(&f),
        "calprog" => parse_calprog(&f),
        "calpend" => parse_calpend(&f),
        "fault" => parse_fault(&f),
        other => return ParseOutcome::Error(ParseError::UnknownType(other.to_string())),
    };

    match parsed {
        Ok(record) => ParseOutcome::Record(record),
        Err(e) => ParseOutcome::Error(e),
    }
}

/// Ordered key/value view. Unknown keys are simply never asked for, which is how the reader
/// tolerates a firmware that has added fields it does not know about.
struct Fields<'a> {
    fields: &'a [(&'a str, &'a str)],
}

impl<'a> Fields<'a> {
    fn raw(&self, key: &'static str) -> Result<&'a str, ParseError> {
        self.fields
            .iter()
            .find(|(k, _)| *k == key)
            .map(|(_, v)| *v)
            .ok_or(ParseError::MissingField(key))
    }

    fn opt_raw(&self, key: &str) -> Option<&'a str> {
        self.fields.iter().find(|(k, _)| *k == key).map(|(_, v)| *v)
    }

    fn bad(key: &str, value: &str) -> ParseError {
        ParseError::BadValue {
            key: key.to_string(),
            value: value.to_string(),
        }
    }

    fn u32(&self, key: &'static str) -> Result<u32, ParseError> {
        let raw = self.raw(key)?;
        raw.parse::<u32>().map_err(|_| Self::bad(key, raw))
    }

    fn u16(&self, key: &'static str) -> Result<u16, ParseError> {
        let raw = self.raw(key)?;
        raw.parse::<u16>().map_err(|_| Self::bad(key, raw))
    }

    fn u8(&self, key: &'static str) -> Result<u8, ParseError> {
        let raw = self.raw(key)?;
        raw.parse::<u8>().map_err(|_| Self::bad(key, raw))
    }

    fn i32(&self, key: &'static str) -> Result<i32, ParseError> {
        let raw = self.raw(key)?;
        raw.parse::<i32>().map_err(|_| Self::bad(key, raw))
    }

    fn i8(&self, key: &'static str) -> Result<i8, ParseError> {
        let raw = self.raw(key)?;
        raw.parse::<i8>().map_err(|_| Self::bad(key, raw))
    }

    fn f32(&self, key: &'static str) -> Result<f32, ParseError> {
        let raw = self.raw(key)?;
        raw.parse::<f32>().map_err(|_| Self::bad(key, raw))
    }

    /// `nan` means the firmware did not evaluate this field; it is absent, not zero.
    fn opt_f32(&self, key: &'static str) -> Result<Option<f32>, ParseError> {
        let raw = self.raw(key)?;
        if raw.eq_ignore_ascii_case("nan") {
            return Ok(None);
        }
        raw.parse::<f32>()
            .map(Some)
            .map_err(|_| Self::bad(key, raw))
    }

    fn bool(&self, key: &'static str) -> Result<bool, ParseError> {
        match self.raw(key)? {
            "1" => Ok(true),
            "0" => Ok(false),
            other => Err(Self::bad(key, other)),
        }
    }

    /// Tri-state: `nan` distinguishes "not evaluated" from "evaluated and false".
    fn opt_bool(&self, key: &'static str) -> Result<Option<bool>, ParseError> {
        match self.raw(key)? {
            "1" => Ok(Some(true)),
            "0" => Ok(Some(false)),
            v if v.eq_ignore_ascii_case("nan") => Ok(None),
            other => Err(Self::bad(key, other)),
        }
    }

    fn hex_u16(&self, key: &'static str) -> Result<u16, ParseError> {
        let raw = self.raw(key)?;
        let body = raw.strip_prefix("0x").or_else(|| raw.strip_prefix("0X"));
        match body {
            Some(hex) => u16::from_str_radix(hex, 16).map_err(|_| Self::bad(key, raw)),
            None => raw.parse::<u16>().map_err(|_| Self::bad(key, raw)),
        }
    }

    fn text(&self, key: &'static str) -> Result<String, ParseError> {
        Ok(percent_decode(self.raw(key)?))
    }

    fn opt_u16(&self, key: &str) -> Option<u16> {
        self.opt_raw(key).and_then(|v| v.parse::<u16>().ok())
    }

    fn opt_i8(&self, key: &str) -> Option<i8> {
        self.opt_raw(key).and_then(|v| v.parse::<i8>().ok())
    }

    fn opt_f32_loose(&self, key: &str) -> Option<f32> {
        self.opt_raw(key)
            .filter(|v| !v.eq_ignore_ascii_case("nan"))
            .and_then(|v| v.parse::<f32>().ok())
    }
}

/// Decodes `%XX` escapes. An invalid escape is left as literal text rather than dropped, so a
/// refusal reason is never silently truncated.
fn percent_decode(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hi = (bytes[i + 1] as char).to_digit(16);
            let lo = (bytes[i + 2] as char).to_digit(16);
            if let (Some(hi), Some(lo)) = (hi, lo) {
                out.push((hi * 16 + lo) as u8);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn mode(f: &Fields, key: &'static str) -> Result<MotionMode, ParseError> {
    let code = f.u8(key)?;
    MotionMode::from_code(code).ok_or_else(|| ParseError::BadValue {
        key: key.to_string(),
        value: code.to_string(),
    })
}

fn stage(f: &Fields) -> Result<CalStage, ParseError> {
    match f.raw("stage")? {
        "align" => Ok(CalStage::Align),
        "charac" => Ok(CalStage::Charac),
        other => Err(Fields::bad("stage", other)),
    }
}

fn parse_id(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::Id(IdRecord {
        firmware_level: f.raw("fw")?.to_string(),
        protocol_version: f.u16("proto")?,
        can_id: f.hex_u16("canid")?,
        motor_count: f.u8("motors")?,
        config_version: f.u16("cfgver")?,
        uptime_ms: f.u32("uptime_ms")?,
    }))
}

fn parse_cal(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::Cal(CalRecord {
        motor: f.u8("m")?,
        aligned: f.bool("aligned")?,
        characterised: f.bool("charac")?,
        pole_pairs: f.u16("pp")?,
        direction: f.i8("dir")?,
        electrical_offset: f.f32("offset")?,
        phase_resistance: f.f32("r")?,
        inductance_d: f.f32("ld")?,
        inductance_q: f.f32("lq")?,
        valid: f.bool("valid")?,
    }))
}

fn parse_cfg(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::Cfg(CfgRecord {
        can_id: f.hex_u16("canid")?,
        bandwidth_requested_hz: f.u16("bw_req")?,
        bandwidth_active_hz: f.u16("bw_act")?,
        bandwidth_clamped: f.bool("bw_clamped")?,
        control_rate_hz: f.u32("rate")?,
        carrier_hz: f.u32("carrier")?,
        decimation: f.u8("decim")?,
        mode: [mode(f, "mode1")?, mode(f, "mode2")?],
        bus_min_mv: f.u16("busmin_mv")?,
        bus_max_mv: f.u16("busmax_mv")?,
        calibrated: f.bool("calibrated")?,
    }))
}

fn parse_motor(f: &Fields) -> Result<Record, ParseError> {
    let pair_fault = f.opt_bool("pairfault")?;
    let limits_mask = f.u32("limits")?;

    // Current firmware evaluates output-voltage limiting and reports pairfault as 0/1. Older
    // flashes still emit pairfault=nan; treat the voltage bit as unevaluated in that case.
    let output_voltage_evaluated = pair_fault.is_some();

    Ok(Record::Motor(MotorRecord {
        motor: f.u8("m")?,
        armed: f.bool("armed")?,
        mode: mode(f, "mode")?,
        position_mrad: f.i32("pos_mrad")?,
        velocity: f.f32("vel")?,
        current_q: f.f32("iq")?,
        timed_out: f.bool("timeout")?,
        limit_causes: LimitCauses::from_mask(limits_mask, output_voltage_evaluated),
        limit_count: f.u16("limitcount")?,
        pair_fault,
    }))
}

fn parse_imp(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::Imp(ImpRecord {
        motor: f.u8("m")?,
        p_des_mrad: f.i32("pdes_mrad")?,
        v_des: f.f32("vdes")?,
        kp: f.f32("kp")?,
        kd: f.f32("kd")?,
        t_ff: f.f32("tff")?,
        position_error: f.f32("perr")?,
        torque_cmd: f.f32("tq")?,
        applied_target_mrad: f.i32("applied_mrad")?,
        capture_generation: f.u8("capgen")?,
        last_seq: f.i32("seq")?,
        pair_fault: f.opt_bool("pairfault")?,
        eligible: f.bool("eligible")?,
        serial_hold: f.opt_raw("hold") == Some("1"),
    }))
}

fn parse_timing(f: &Fields) -> Result<Record, ParseError> {
    let fault = match f.raw("fault")? {
        "none" => TimingFault::None,
        "overrun" => TimingFault::Overrun,
        "rate" => TimingFault::Rate,
        other => return Err(Fields::bad("fault", other)),
    };
    Ok(Record::Timing(TimingRecord {
        rate_nominal_hz: f.u32("rate_nom")?,
        rate_measured_hz: f.f32("rate_meas")?,
        period_us: f.f32("period_us")?,
        cycles: f.u32("cycles")?,
        overruns: f.u32("overruns")?,
        consecutive_overruns: f.u16("consec")?,
        last_cycle_us: f.u32("last_us")?,
        worst_cycle_us: f.u32("worst_us")?,
        duty_percent: f.f32("duty")?,
        fault,
    }))
}

fn parse_bus(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::Bus(BusRecord {
        millivolts: f.u32("mv")?,
        protection_active: f.bool("protect")?,
    }))
}

fn parse_ack(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::Ack(AckRecord {
        tag: f.u8("tag")?,
        command: f.raw("cmd")?.to_string(),
        ok: f.bool("ok")?,
        reason: f.text("reason")?,
    }))
}

fn parse_calprog(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::CalProg(CalProgRecord {
        motor: f.u8("m")?,
        stage: stage(f)?,
        percent: f.u8("pct")?,
        energised: f.bool("energised")?,
    }))
}

fn parse_calpend(f: &Fields) -> Result<Record, ParseError> {
    Ok(Record::CalPend(CalPendRecord {
        motor: f.u8("m")?,
        stage: stage(f)?,
        pole_pairs: f.opt_u16("pp"),
        direction: f.opt_i8("dir"),
        electrical_offset: f.opt_f32_loose("offset"),
        phase_resistance: f.opt_f32_loose("r"),
        inductance_d: f.opt_f32_loose("ld"),
        inductance_q: f.opt_f32_loose("lq"),
    }))
}

fn parse_fault(f: &Fields) -> Result<Record, ParseError> {
    let kind = match f.raw("kind")? {
        "calibration" => FaultKind::Calibration,
        "timing" => FaultKind::Timing,
        "bus" => FaultKind::Bus,
        "protocol" => FaultKind::Protocol,
        other => return Err(Fields::bad("kind", other)),
    };
    Ok(Record::Fault(FaultRecord {
        kind,
        reason: f.text("reason")?,
        cooldown_ms: f.u32("cooldown_ms")?,
    }))
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURES: &str = include_str!("../../tests/fixtures/records.txt");

    /// Splits the shared corpus into its `=== NAME ===` sections, dropping comments and blanks.
    fn section(name: &str) -> Vec<&'static str> {
        let mut out = Vec::new();
        let mut inside = false;
        for line in FIXTURES.lines() {
            if let Some(header) = line.strip_prefix("=== ") {
                inside = header.trim_end_matches(" ===") == name;
                continue;
            }
            if !inside {
                continue;
            }
            if line.trim().is_empty() {
                continue;
            }
            // Comments start with '#' but are never '#V13'.
            if line.starts_with('#') && !line.starts_with("#V13") {
                continue;
            }
            out.push(line);
        }
        out
    }

    #[test]
    fn every_valid_fixture_parses() {
        let lines = section("VALID RECORDS");
        assert!(lines.len() >= 16, "fixture corpus shrank: {}", lines.len());
        for line in lines {
            match parse_line(line) {
                ParseOutcome::Record(_) => {}
                other => panic!("expected a record for {line:?}, got {other:?}"),
            }
        }
    }

    #[test]
    fn all_eleven_record_types_are_covered() {
        let mut seen = std::collections::HashSet::new();
        for line in section("VALID RECORDS") {
            if let ParseOutcome::Record(r) = parse_line(line) {
                seen.insert(std::mem::discriminant(&r));
            }
        }
        assert_eq!(seen.len(), 11, "expected all 11 record types in the corpus");
    }

    #[test]
    fn firmware_prose_is_ignored_not_misparsed() {
        for line in section("MUST BE IGNORED: FIRMWARE PROSE") {
            assert_eq!(
                parse_line(line),
                ParseOutcome::Ignored,
                "prose was not ignored: {line:?}"
            );
        }
    }

    #[test]
    fn unsupported_versions_are_refused() {
        for line in section("MUST BE REJECTED: UNSUPPORTED VERSION") {
            match parse_line(line) {
                ParseOutcome::Error(ParseError::UnsupportedVersion(_)) => {}
                other => panic!("expected UnsupportedVersion for {line:?}, got {other:?}"),
            }
        }
    }

    #[test]
    fn malformed_lines_are_rejected_without_panicking() {
        for line in section("MUST BE REJECTED: MALFORMED") {
            match parse_line(line) {
                ParseOutcome::Error(_) => {}
                other => panic!("expected an error for {line:?}, got {other:?}"),
            }
        }
    }

    #[test]
    fn unknown_keys_are_tolerated() {
        for line in section("UNKNOWN KEY TOLERATED") {
            match parse_line(line) {
                ParseOutcome::Record(Record::Bus(b)) => assert_eq!(b.millivolts, 12040),
                other => panic!("expected a bus record for {line:?}, got {other:?}"),
            }
        }
    }

    #[test]
    fn hundred_revolution_position_survives_round_trip() {
        // The SC-013 safety anchor. A 16-bit field could not represent this, which is why the
        // position field is a signed 32-bit milliradian value.
        let line = "#V13 v=1 t=motor m=1 armed=0 mode=0 pos_mrad=628319 vel=0.0 iq=0.0 timeout=0 limits=0 limitcount=0 pairfault=nan";
        match parse_line(line) {
            ParseOutcome::Record(Record::Motor(m)) => assert_eq!(m.position_mrad, 628319),
            other => panic!("unexpected {other:?}"),
        }

        let negative = line.replace("628319", "-628319");
        match parse_line(&negative) {
            ParseOutcome::Record(Record::Motor(m)) => assert_eq!(m.position_mrad, -628319),
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn degraded_fields_are_unavailable_not_false() {
        let line = "#V13 v=1 t=motor m=1 armed=0 mode=0 pos_mrad=0 vel=0.0 iq=0.0 timeout=0 limits=0 limitcount=0 pairfault=nan";
        match parse_line(line) {
            ParseOutcome::Record(Record::Motor(m)) => {
                assert_eq!(
                    m.pair_fault, None,
                    "pair fault must be unavailable, not false"
                );
                assert_eq!(
                    m.limit_causes.output_voltage, None,
                    "output-voltage cause must be unavailable, not false"
                );
                assert!(!m.limit_causes.current);
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn limit_mask_decodes_current_and_bus_but_not_output_voltage() {
        // limits=5 is current (bit 0) + bus voltage (bit 2). pairfault=nan means the flash
        // has not evaluated the voltage bit, so it stays unavailable.
        let line = "#V13 v=1 t=motor m=2 armed=1 mode=1 pos_mrad=0 vel=0.0 iq=0.0 timeout=0 limits=5 limitcount=3 pairfault=nan";
        match parse_line(line) {
            ParseOutcome::Record(Record::Motor(m)) => {
                assert!(m.limit_causes.current);
                assert!(m.limit_causes.bus_voltage);
                assert_eq!(m.limit_causes.output_voltage, None);
                assert_eq!(m.limit_count, 3);
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn output_voltage_is_decoded_when_pairfault_is_evaluated() {
        let line = "#V13 v=1 t=motor m=1 armed=0 mode=1 pos_mrad=0 vel=0.0 iq=0.0 timeout=0 limits=2 limitcount=1 pairfault=0";
        match parse_line(line) {
            ParseOutcome::Record(Record::Motor(m)) => {
                assert_eq!(m.pair_fault, Some(false));
                assert_eq!(m.limit_causes.output_voltage, Some(true));
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn impedance_record_round_trips() {
        let line = "#V13 v=1 t=imp m=1 pdes_mrad=628319 vdes=0.012 kp=12.000 kd=0.300 tff=0.0100 perr=0.00359 tq=0.0431 applied_mrad=628319 capgen=4 seq=42 pairfault=0 eligible=1 hold=1";
        match parse_line(line) {
            ParseOutcome::Record(Record::Imp(r)) => {
                assert_eq!(r.p_des_mrad, 628319);
                assert_eq!(r.last_seq, 42);
                assert_eq!(r.pair_fault, Some(false));
                assert!(r.eligible);
                assert!(r.serial_hold);
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn refusal_reason_is_percent_decoded() {
        let line =
            "#V13 v=1 t=ack tag=8 cmd=B2500 ok=0 reason=disarm%20both%20motors%20first%20%28D0%29";
        match parse_line(line) {
            ParseOutcome::Record(Record::Ack(a)) => {
                assert_eq!(a.reason, "disarm both motors first (D0)");
                assert!(!a.ok);
                assert_eq!(a.tag, 8);
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn successful_ack_has_an_empty_reason() {
        match parse_line("#V13 v=1 t=ack tag=7 cmd=CY ok=1 reason=") {
            ParseOutcome::Record(Record::Ack(a)) => {
                assert!(a.ok);
                assert!(a.reason.is_empty());
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn version_must_be_the_first_field() {
        match parse_line("#V13 t=bus v=1 mv=12040 protect=0") {
            ParseOutcome::Error(ParseError::MissingVersion) => {}
            other => panic!("expected MissingVersion, got {other:?}"),
        }
    }

    #[test]
    fn duplicate_field_is_ambiguous_and_rejected() {
        match parse_line("#V13 v=1 t=bus mv=12040 mv=99999 protect=0") {
            ParseOutcome::Error(ParseError::DuplicateField(k)) => assert_eq!(k, "mv"),
            other => panic!("expected DuplicateField, got {other:?}"),
        }
    }

    #[test]
    fn unknown_record_type_is_rejected() {
        match parse_line("#V13 v=1 t=somethingnew mv=1") {
            ParseOutcome::Error(ParseError::UnknownType(t)) => assert_eq!(t, "somethingnew"),
            other => panic!("expected UnknownType, got {other:?}"),
        }
    }

    #[test]
    fn missing_required_field_is_rejected() {
        match parse_line("#V13 v=1 t=bus protect=0") {
            ParseOutcome::Error(ParseError::MissingField("mv")) => {}
            other => panic!("expected MissingField(mv), got {other:?}"),
        }
    }

    #[test]
    fn prefix_must_be_exact() {
        // A line that merely contains the prefix, or extends it, is not a record.
        assert_eq!(
            parse_line("prefixed #V13 v=1 t=bus mv=1 protect=0"),
            ParseOutcome::Ignored
        );
        assert_eq!(
            parse_line("#V13X v=1 t=bus mv=1 protect=0"),
            ParseOutcome::Ignored
        );
        assert_eq!(parse_line(""), ParseOutcome::Ignored);
        assert_eq!(parse_line("   "), ParseOutcome::Ignored);
    }

    #[test]
    fn bare_prefix_is_an_error_not_a_record() {
        match parse_line("#V13") {
            ParseOutcome::Error(ParseError::MissingVersion) => {}
            other => panic!("expected MissingVersion, got {other:?}"),
        }
    }

    #[test]
    fn line_endings_are_tolerated() {
        for line in [
            "#V13 v=1 t=bus mv=12040 protect=0\n",
            "#V13 v=1 t=bus mv=12040 protect=0\r\n",
            "  #V13 v=1 t=bus mv=12040 protect=0  ",
        ] {
            match parse_line(line) {
                ParseOutcome::Record(Record::Bus(b)) => assert_eq!(b.millivolts, 12040),
                other => panic!("unexpected {other:?} for {line:?}"),
            }
        }
    }

    #[test]
    fn calibration_values_parse_to_expected_magnitudes() {
        let line = "#V13 v=1 t=cal m=1 aligned=1 charac=1 pp=7 dir=1 offset=2.094395 r=0.084000 ld=0.000125 lq=0.000131 valid=1";
        match parse_line(line) {
            ParseOutcome::Record(Record::Cal(c)) => {
                assert_eq!(c.pole_pairs, 7);
                assert_eq!(c.direction, 1);
                assert!((c.phase_resistance - 0.084).abs() < 1e-6);
                // Both axis inductances are always reported; the reference design's single field
                // would have misrepresented the device.
                assert!((c.inductance_d - 0.000125).abs() < 1e-9);
                assert!((c.inductance_q - 0.000131).abs() < 1e-9);
                assert_ne!(c.inductance_d, c.inductance_q);
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn clamped_bandwidth_is_visible_in_cfg() {
        let line = "#V13 v=1 t=cfg canid=0x201 bw_req=2500 bw_act=1000 bw_clamped=1 rate=10000 carrier=20000 decim=2 mode1=0 mode2=0 busmin_mv=7000 busmax_mv=24000 calibrated=0";
        match parse_line(line) {
            ParseOutcome::Record(Record::Cfg(c)) => {
                assert_eq!(c.can_id, 0x201);
                assert_eq!(c.bandwidth_requested_hz, 2500);
                assert_eq!(c.bandwidth_active_hz, 1000);
                assert!(c.bandwidth_clamped);
                // The smallest-N rule: 1000 Hz -> 10 kHz rate -> N=2 -> 20 kHz carrier.
                assert_eq!(c.control_rate_hz, 10_000);
                assert_eq!(c.carrier_hz, 20_000);
                assert_eq!(c.decimation, 2);
                assert_eq!(c.mode, [MotionMode::Velocity, MotionMode::Velocity]);
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn calpend_carries_only_its_stage_fields() {
        match parse_line("#V13 v=1 t=calpend m=1 stage=align pp=7 dir=1 offset=2.094395") {
            ParseOutcome::Record(Record::CalPend(p)) => {
                assert_eq!(p.stage, CalStage::Align);
                assert_eq!(p.pole_pairs, Some(7));
                assert_eq!(p.phase_resistance, None);
            }
            other => panic!("unexpected {other:?}"),
        }
        match parse_line("#V13 v=1 t=calpend m=1 stage=charac r=0.084 ld=0.000125 lq=0.000131") {
            ParseOutcome::Record(Record::CalPend(p)) => {
                assert_eq!(p.stage, CalStage::Charac);
                assert_eq!(p.pole_pairs, None);
                assert!(p.phase_resistance.is_some());
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn fault_reason_and_cooldown_survive() {
        match parse_line(
            "#V13 v=1 t=fault kind=calibration reason=over-current%20abort cooldown_ms=28500",
        ) {
            ParseOutcome::Record(Record::Fault(f)) => {
                assert_eq!(f.kind, FaultKind::Calibration);
                assert_eq!(f.reason, "over-current abort");
                assert_eq!(f.cooldown_ms, 28_500);
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn never_panics_on_arbitrary_input() {
        let nasty = [
            "#V13 v=1 t=bus mv=999999999999999999999999 protect=0",
            "#V13 v=99999999999 t=bus mv=1 protect=0",
            "#V13 v=1 t=ack tag=300 cmd=X ok=1 reason=",
            "#V13 v=1 t=ack tag=1 cmd=X ok=2 reason=",
            "#V13 v=1 t=cal m=1 aligned=1 charac=1 pp=7 dir=1 offset=abc r=1 ld=1 lq=1 valid=1",
            "#V13 v=1 t=motor m=1 armed=0 mode=9 pos_mrad=0 vel=0 iq=0 timeout=0 limits=0 limitcount=0 pairfault=nan",
            "#V13 v=1 t=bus mv=12040 protect=maybe",
            "#V13 v=1 t=ack tag=1 cmd=X ok=1 reason=%",
            "#V13 v=1 t=ack tag=1 cmd=X ok=1 reason=%2",
            "#V13 v=1 t=ack tag=1 cmd=X ok=1 reason=%ZZ",
            "#V13 = = =",
            "#V13 v=1 t=bus =",
        ];
        for line in nasty {
            let _ = parse_line(line);
        }
    }

    #[test]
    fn percent_decode_leaves_invalid_escapes_literal() {
        assert_eq!(percent_decode("a%20b"), "a b");
        assert_eq!(percent_decode("100%"), "100%");
        assert_eq!(percent_decode("%ZZ"), "%ZZ");
        assert_eq!(percent_decode("%2"), "%2");
        assert_eq!(percent_decode("%28D0%29"), "(D0)");
    }
}
