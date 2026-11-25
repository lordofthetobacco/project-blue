//! Structured logging to stdout: one JSON object (or one `key=value` text
//! line) per event.
//!
//! Lines are written straight into the locked stdout buffer, so logging
//! does not allocate.

use std::fmt;
use std::io::Write as _;
use std::sync::atomic::{AtomicU8, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

/// Output format.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    /// One JSON object per line.
    Json,
    /// Human-readable `time LEVEL event key=value ...`.
    Text,
}

/// Severity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    /// Diagnostic detail.
    Debug,
    /// Normal operation.
    Info,
    /// Something is off but the server continues.
    Warn,
    /// An operation failed.
    Error,
}

impl Level {
    fn as_str(self) -> &'static str {
        match self {
            Self::Debug => "debug",
            Self::Info => "info",
            Self::Warn => "warn",
            Self::Error => "error",
        }
    }
}

/// One field value.
#[derive(Clone, Copy, Debug)]
pub enum Value<'a> {
    /// Text.
    Str(&'a str),
    /// Unsigned integer.
    U64(u64),
    /// Signed integer.
    I64(i64),
    /// Float.
    F64(f64),
    /// Boolean.
    Bool(bool),
}

impl<'a> From<&'a str> for Value<'a> {
    fn from(v: &'a str) -> Self {
        Self::Str(v)
    }
}
impl From<u64> for Value<'_> {
    fn from(v: u64) -> Self {
        Self::U64(v)
    }
}
impl From<u32> for Value<'_> {
    fn from(v: u32) -> Self {
        Self::U64(u64::from(v))
    }
}
impl From<u16> for Value<'_> {
    fn from(v: u16) -> Self {
        Self::U64(u64::from(v))
    }
}
impl From<u8> for Value<'_> {
    fn from(v: u8) -> Self {
        Self::U64(u64::from(v))
    }
}
impl From<usize> for Value<'_> {
    fn from(v: usize) -> Self {
        Self::U64(v as u64)
    }
}
impl From<i64> for Value<'_> {
    fn from(v: i64) -> Self {
        Self::I64(v)
    }
}
impl From<i32> for Value<'_> {
    fn from(v: i32) -> Self {
        Self::I64(i64::from(v))
    }
}
impl From<f64> for Value<'_> {
    fn from(v: f64) -> Self {
        Self::F64(v)
    }
}
impl From<bool> for Value<'_> {
    fn from(v: bool) -> Self {
        Self::Bool(v)
    }
}

static FORMAT: AtomicU8 = AtomicU8::new(0);

/// Selects the output format for the rest of the process.
pub fn set_format(format: Format) {
    FORMAT.store(
        match format {
            Format::Json => 0,
            Format::Text => 1,
        },
        Ordering::Relaxed,
    );
}

fn format() -> Format {
    if FORMAT.load(Ordering::Relaxed) == 0 {
        Format::Json
    } else {
        Format::Text
    }
}

/// UTC civil time from seconds since the epoch (Howard Hinnant's
/// `civil_from_days`).
fn civil(secs: u64) -> (i64, u32, u32, u32, u32, u32) {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let year = yoe + era * 400 + i64::from(month <= 2);
    (
        year,
        month,
        day,
        (rem / 3600) as u32,
        ((rem % 3600) / 60) as u32,
        (rem % 60) as u32,
    )
}

struct Timestamp;

impl fmt::Display for Timestamp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default();
        let (y, mo, d, h, mi, s) = civil(now.as_secs());
        write!(
            f,
            "{y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{s:02}.{:03}Z",
            now.subsec_millis()
        )
    }
}

struct JsonStr<'a>(&'a str);

impl fmt::Display for JsonStr<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use fmt::Write as _;
        f.write_char('"')?;
        for c in self.0.chars() {
            match c {
                '"' => f.write_str("\\\"")?,
                '\\' => f.write_str("\\\\")?,
                '\n' => f.write_str("\\n")?,
                '\r' => f.write_str("\\r")?,
                '\t' => f.write_str("\\t")?,
                c if (c as u32) < 0x20 => write!(f, "\\u{:04x}", c as u32)?,
                c => f.write_char(c)?,
            }
        }
        f.write_char('"')
    }
}

struct JsonValue<'a>(Value<'a>);

impl fmt::Display for JsonValue<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Value::Str(s) => JsonStr(s).fmt(f),
            Value::U64(v) => write!(f, "{v}"),
            Value::I64(v) => write!(f, "{v}"),
            Value::F64(v) if v.is_finite() => write!(f, "{v}"),
            Value::F64(_) => f.write_str("null"),
            Value::Bool(v) => write!(f, "{v}"),
        }
    }
}

struct TextValue<'a>(Value<'a>);

impl fmt::Display for TextValue<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Value::Str(s) if s.is_empty() || s.contains([' ', '"', '=']) => JsonStr(s).fmt(f),
            Value::Str(s) => f.write_str(s),
            Value::U64(v) => write!(f, "{v}"),
            Value::I64(v) => write!(f, "{v}"),
            Value::F64(v) => write!(f, "{v}"),
            Value::Bool(v) => write!(f, "{v}"),
        }
    }
}

/// Writes one log line. Output errors are ignored (logging must never take
/// the server down).
pub fn log(level: Level, event: &str, fields: &[(&str, Value<'_>)]) {
    let stdout = std::io::stdout();
    let mut out = stdout.lock();
    let _ = match format() {
        Format::Json => write_json(&mut out, level, event, fields),
        Format::Text => write_text(&mut out, level, event, fields),
    };
    let _ = out.flush();
}

fn write_json(
    out: &mut impl std::io::Write,
    level: Level,
    event: &str,
    fields: &[(&str, Value<'_>)],
) -> std::io::Result<()> {
    write!(
        out,
        "{{\"ts\":\"{}\",\"level\":\"{}\",\"event\":{}",
        Timestamp,
        level.as_str(),
        JsonStr(event)
    )?;
    for (key, value) in fields {
        write!(out, ",{}:{}", JsonStr(key), JsonValue(*value))?;
    }
    out.write_all(b"}\n")
}

fn write_text(
    out: &mut impl std::io::Write,
    level: Level,
    event: &str,
    fields: &[(&str, Value<'_>)],
) -> std::io::Result<()> {
    write!(
        out,
        "{} {:<5} {event}",
        Timestamp,
        level.as_str().to_uppercase()
    )?;
    for (key, value) in fields {
        write!(out, " {key}={}", TextValue(*value))?;
    }
    out.write_all(b"\n")
}

/// Logs at info level.
pub fn info(event: &str, fields: &[(&str, Value<'_>)]) {
    log(Level::Info, event, fields);
}

/// Logs at warn level.
pub fn warn(event: &str, fields: &[(&str, Value<'_>)]) {
    log(Level::Warn, event, fields);
}

/// Logs at error level.
pub fn error(event: &str, fields: &[(&str, Value<'_>)]) {
    log(Level::Error, event, fields);
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]

    use super::*;

    #[test]
    fn civil_dates() {
        assert_eq!(civil(0), (1970, 1, 1, 0, 0, 0));
        // 2026-11-24T12:34:56Z
        assert_eq!(civil(1_795_523_696), (2026, 11, 24, 12, 34, 56));
        // Leap day 2024-02-29.
        assert_eq!(civil(1_709_164_800), (2024, 2, 29, 0, 0, 0));
    }

    #[test]
    fn json_line() {
        let mut out = Vec::new();
        write_json(
            &mut out,
            Level::Warn,
            "player_joined",
            &[
                ("name", "a\"b\n".into()),
                ("id", 3u8.into()),
                ("x", f64::NAN.into()),
                ("ok", true.into()),
                ("d", (-2i32).into()),
            ],
        )
        .unwrap();
        let line = String::from_utf8(out).unwrap();
        assert!(line.starts_with("{\"ts\":\""));
        let tail = r#","level":"warn","event":"player_joined","name":"a\"b\n","id":3,"x":null,"ok":true,"d":-2}"#;
        assert!(line.ends_with(&format!("{tail}\n")), "{line}");
    }

    #[test]
    fn text_line() {
        let mut out = Vec::new();
        write_text(
            &mut out,
            Level::Info,
            "start",
            &[("port", 27015u16.into()), ("name", "two words".into())],
        )
        .unwrap();
        let line = String::from_utf8(out).unwrap();
        assert!(line.ends_with(" INFO  start port=27015 name=\"two words\"\n"));
    }
}
