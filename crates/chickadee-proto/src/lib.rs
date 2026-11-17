// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

//! chickadeeBRRD host-to-stick protocol, version 0.
//!
//! Message types and the line codec only. No I/O lives here, so the same
//! crate can sit behind the CLI today and an Android binding later.
//!
//! Wire format: one JSON object per line, UTF-8, terminated by `\n`.
//! See `docs/protocol.md` for the full specification.

use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

/// Protocol version this crate speaks.
pub const PROTO_VERSION: u32 = 0;

/// Longest line either side may send, in bytes, including the newline.
pub const MAX_LINE_LEN: usize = 1024;

/// USB identity of the ESP32-C5 built-in USB Serial/JTAG controller.
pub const USB_VID: u16 = 0x303a;
pub const USB_PID: u16 = 0x1001;

/// A command the host can send.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "cmd")]
pub enum Command {
    /// Identify the stick: firmware, protocol version, board, capabilities.
    #[serde(rename = "hello")]
    Hello,
    /// Current state and uptime.
    #[serde(rename = "status")]
    Status,
    /// Show a short text message on the stick's screen.
    #[serde(rename = "display.text")]
    DisplayText { text: String },
    /// Begin a transmit session. The stick grants a lease that must be renewed.
    #[serde(rename = "tx.start")]
    TxStart {
        signal: String,
        /// Where the stick should say it is, in degrees. Required for `bt4`.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        lat: Option<f64>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        lon: Option<f64>,
        /// The host's clock, in Unix seconds. Required for `bt4`: the stick
        /// has no clock of its own.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        time: Option<f64>,
        /// Transmit power in dBm. Absent means the stick's default, its lowest.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        power_dbm: Option<i32>,
        /// Time limit for this session. Absent means the stick's default.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        max_ms: Option<u64>,
    },
    /// Keep the current session alive.
    #[serde(rename = "lease.renew")]
    LeaseRenew {
        /// The host's clock, in Unix seconds, so the stick's copy does not drift.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        time: Option<f64>,
    },
    /// End the current session. Succeeds even when none is running.
    #[serde(rename = "tx.stop")]
    TxStop,
}

impl Command {
    /// `tx.start` for a signal that needs nothing but its name.
    pub fn tx_start(signal: &str) -> Self {
        Command::TxStart { signal: signal.to_string(), lat: None, lon: None, time: None, power_dbm: None, max_ms: None }
    }
}

/// A request line: a command plus the id the response must echo.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Request {
    pub id: u32,
    #[serde(flatten)]
    pub cmd: Command,
}

impl Request {
    /// Encode as a single wire line, newline included.
    pub fn encode(&self) -> String {
        let mut s = serde_json::to_string(self).expect("request serializes");
        s.push('\n');
        s
    }
}

/// A response line. Command-specific fields are kept in `data`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Response {
    pub id: u32,
    pub ok: bool,
    /// Machine-readable error code, present when `ok` is false.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub err: Option<String>,
    /// Human-readable detail for the error.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub msg: Option<String>,
    #[serde(flatten)]
    pub data: Map<String, Value>,
}

impl Response {
    /// Read the command-specific fields as a typed payload.
    pub fn payload<T: DeserializeOwned>(&self) -> Result<T, serde_json::Error> {
        serde_json::from_value(Value::Object(self.data.clone()))
    }
}

/// An unsolicited line from the stick, such as `{"ev":"boot",...}`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Event {
    pub ev: String,
    #[serde(flatten)]
    pub data: Map<String, Value>,
}

/// Payload of a successful `hello`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Hello {
    #[serde(default)]
    pub device: String,
    #[serde(default)]
    pub board: String,
    #[serde(default)]
    pub fw: String,
    pub proto: u32,
    #[serde(default)]
    pub caps: Vec<String>,
    /// Signal types `tx.start` accepts.
    #[serde(default)]
    pub signals: Vec<String>,
    /// Limits the firmware enforces.
    #[serde(default)]
    pub limits: Limits,
    /// Who the stick says it is on the air. Fixed in the firmware.
    #[serde(default)]
    pub rid: Option<RidIdentity>,
}

/// Limits the firmware enforces, as reported by `hello`. For the durations,
/// zero means unknown; the power fields are absent on firmware with no radio.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Limits {
    /// A session stops if this long passes without `lease.renew`.
    #[serde(default)]
    pub lease_ms: u64,
    /// A session stops after this long unless `tx.start` asked for longer.
    /// Zero on firmware older than 0.4, where `session_max_ms` was the only limit.
    #[serde(default)]
    pub session_default_ms: u64,
    /// The longest session `tx.start` can ask for.
    #[serde(default)]
    pub session_max_ms: u64,
    /// Transmit power, in dBm: the lowest, the one used when none is asked
    /// for, the highest this firmware allows, and the step between settings.
    #[serde(default)]
    pub power_min_dbm: Option<i32>,
    #[serde(default)]
    pub power_default_dbm: Option<i32>,
    #[serde(default)]
    pub power_max_dbm: Option<i32>,
    #[serde(default)]
    pub power_step_db: Option<i32>,
}

/// The identity the stick broadcasts in Remote ID messages.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RidIdentity {
    /// The Basic ID type, e.g. `specific_session_id`.
    #[serde(default)]
    pub id_type: String,
    /// The Basic ID text.
    #[serde(default)]
    pub id: String,
    /// The Self-ID description text.
    #[serde(default)]
    pub self_id: String,
}

/// Payload of a successful `tx.start` or `lease.renew`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Lease {
    pub lease_ms: u64,
    #[serde(default)]
    pub session_remaining_ms: u64,
    /// The power the stick will transmit at. `tx.start` for a radio signal only.
    #[serde(default)]
    pub power_dbm: Option<i32>,
}

/// Payload of a successful `status`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Status {
    /// `idle` or `transmitting`.
    pub state: String,
    #[serde(default)]
    pub uptime_ms: u64,
    /// Present while transmitting.
    #[serde(default)]
    pub signal: Option<String>,
    #[serde(default)]
    pub session_remaining_ms: Option<u64>,
    #[serde(default)]
    pub lease_remaining_ms: Option<u64>,
    /// Present while transmitting a radio signal: the power and the position
    /// being broadcast.
    #[serde(default)]
    pub power_dbm: Option<i32>,
    #[serde(default)]
    pub lat: Option<f64>,
    #[serde(default)]
    pub lon: Option<f64>,
    /// Why the last session ended: `stopped`, `lease_expired`, `limit` or `error`.
    #[serde(default)]
    pub last_stop: Option<String>,
}

/// What one line from the stick turned out to be.
#[derive(Debug, Clone, PartialEq)]
pub enum Line {
    Response(Response),
    Event(Event),
    /// Anything that is not a protocol object: boot ROM chatter, ESP-IDF logs.
    Log(String),
}

/// Classify one line received from the stick.
///
/// The stick's logs share the serial stream with the protocol, so a line that
/// does not parse as a protocol object is passed through as `Line::Log`
/// instead of being treated as an error.
pub fn classify(raw: &str) -> Line {
    let line = raw.trim_end_matches(['\r', '\n']).trim_start();
    if line.starts_with('{') {
        if let Ok(Value::Object(obj)) = serde_json::from_str::<Value>(line) {
            if obj.get("ev").is_some_and(Value::is_string) {
                if let Ok(ev) = serde_json::from_value(Value::Object(obj.clone())) {
                    return Line::Event(ev);
                }
            } else if obj.contains_key("id") && obj.contains_key("ok") {
                if let Ok(resp) = serde_json::from_value(Value::Object(obj.clone())) {
                    return Line::Response(resp);
                }
            }
        }
    }
    Line::Log(raw.trim_end_matches(['\r', '\n']).to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requests_encode_to_the_documented_lines() {
        let cases = [
            (Command::Hello, "{\"id\":1,\"cmd\":\"hello\"}\n"),
            (Command::Status, "{\"id\":1,\"cmd\":\"status\"}\n"),
            (Command::DisplayText { text: "hi".into() }, "{\"id\":1,\"cmd\":\"display.text\",\"text\":\"hi\"}\n"),
            (Command::tx_start("led"), "{\"id\":1,\"cmd\":\"tx.start\",\"signal\":\"led\"}\n"),
            (
                Command::TxStart {
                    signal: "bt4".into(),
                    lat: Some(40.9),
                    lon: Some(-95.6),
                    time: Some(1794000000.5),
                    power_dbm: Some(-6),
                    max_ms: Some(600_000),
                },
                "{\"id\":1,\"cmd\":\"tx.start\",\"signal\":\"bt4\",\"lat\":40.9,\"lon\":-95.6,\
                 \"time\":1794000000.5,\"power_dbm\":-6,\"max_ms\":600000}\n",
            ),
            (Command::LeaseRenew { time: None }, "{\"id\":1,\"cmd\":\"lease.renew\"}\n"),
            (
                Command::LeaseRenew { time: Some(1794000001.0) },
                "{\"id\":1,\"cmd\":\"lease.renew\",\"time\":1794000001.0}\n",
            ),
            (Command::TxStop, "{\"id\":1,\"cmd\":\"tx.stop\"}\n"),
        ];
        for (cmd, want) in cases {
            assert_eq!(Request { id: 1, cmd }.encode(), want);
        }
    }

    #[test]
    fn requests_round_trip() {
        let req = Request { id: 42, cmd: Command::DisplayText { text: "chick-a-dee".into() } };
        let back: Request = serde_json::from_str(req.encode().trim_end()).unwrap();
        assert_eq!(back, req);
    }

    #[test]
    fn hello_payload_is_typed() {
        let line = r#"{"id":3,"ok":true,"device":"chickadeeBRRD","board":"lilygo-t-dongle-c5","fw":"0.1.0","proto":0,"caps":["display.text"]}"#;
        let Line::Response(resp) = classify(line) else { panic!("not a response") };
        assert!(resp.ok);
        let hello: Hello = resp.payload().unwrap();
        assert_eq!(hello.proto, PROTO_VERSION);
        assert_eq!(hello.caps, vec!["display.text"]);
        // Older firmware omits these; they default to empty and zero.
        assert!(hello.signals.is_empty());
        assert_eq!(hello.limits, Limits::default());
    }

    #[test]
    fn hello_with_transmit_limits() {
        let line = r#"{"id":3,"ok":true,"device":"chickadeeBRRD","board":"b","fw":"0.3.0","proto":0,"caps":["display.text","tx"],"signals":["led"],"limits":{"lease_ms":3000,"session_max_ms":300000,"text_max":64}}"#;
        let Line::Response(resp) = classify(line) else { panic!("not a response") };
        let hello: Hello = resp.payload().unwrap();
        assert_eq!(hello.signals, vec!["led"]);
        assert_eq!(hello.limits, Limits { lease_ms: 3000, session_max_ms: 300_000, ..Limits::default() });
        assert_eq!(hello.rid, None);
    }

    #[test]
    fn hello_with_a_radio() {
        let line = r#"{"id":1,"ok":true,"device":"chickadeeBRRD","board":"b","fw":"0.4.0","proto":0,"caps":["display.text","tx","led.selftest"],"signals":["led","bt4"],"limits":{"lease_ms":3000,"session_default_ms":300000,"session_max_ms":3600000,"power_min_dbm":-24,"power_default_dbm":-24,"power_max_dbm":0,"power_step_db":3,"text_max":64},"rid":{"id_type":"specific_session_id","id":"CHICKADEEBRRD-TEST","self_id":"chickadeeBRRD:TEST"}}"#;
        let Line::Response(resp) = classify(line) else { panic!("not a response") };
        let hello: Hello = resp.payload().unwrap();
        assert_eq!(hello.signals, vec!["led", "bt4"]);
        assert_eq!(hello.limits.session_default_ms, 300_000);
        assert_eq!(hello.limits.session_max_ms, 3_600_000);
        assert_eq!(hello.limits.power_default_dbm, Some(-24));
        assert_eq!(hello.limits.power_max_dbm, Some(0));
        assert_eq!(hello.rid.unwrap().id, "CHICKADEEBRRD-TEST");
    }

    #[test]
    fn status_while_broadcasting() {
        let line = r#"{"id":4,"ok":true,"state":"transmitting","uptime_ms":2200,"signal":"bt4","session_remaining_ms":298800,"lease_remaining_ms":2800,"power_dbm":-24,"lat":40.9,"lon":-95.6}"#;
        let Line::Response(resp) = classify(line) else { panic!("not a response") };
        let status: Status = resp.payload().unwrap();
        assert_eq!(status.power_dbm, Some(-24));
        assert_eq!(status.lat, Some(40.9));
        assert_eq!(status.lon, Some(-95.6));
    }

    #[test]
    fn status_while_transmitting() {
        let line = r#"{"id":4,"ok":true,"state":"transmitting","uptime_ms":9000,"signal":"led","session_remaining_ms":297000,"lease_remaining_ms":2100}"#;
        let Line::Response(resp) = classify(line) else { panic!("not a response") };
        let status: Status = resp.payload().unwrap();
        assert_eq!(status.signal.as_deref(), Some("led"));
        assert_eq!(status.session_remaining_ms, Some(297_000));
        assert_eq!(status.last_stop, None);
    }

    #[test]
    fn error_response_carries_code_and_message() {
        let line = r#"{"id":9,"ok":false,"err":"unknown_cmd","msg":"no such command: fly"}"#;
        let Line::Response(resp) = classify(line) else { panic!("not a response") };
        assert!(!resp.ok);
        assert_eq!(resp.err.as_deref(), Some("unknown_cmd"));
        assert_eq!(resp.msg.as_deref(), Some("no such command: fly"));
    }

    /// The shared vectors are the contract every implementation must pass.
    #[test]
    fn device_line_vectors_classify_as_labelled() {
        let vectors = include_str!("../../../testdata/vectors/v0-device-lines.jsonl");
        let mut n = 0;
        for row in vectors.lines().filter(|l| !l.trim().is_empty()) {
            let v: Value = serde_json::from_str(row).expect("vector row is JSON");
            let line = v["line"].as_str().unwrap();
            let want = v["kind"].as_str().unwrap();
            let got = match classify(line) {
                Line::Response(_) => "response",
                Line::Event(_) => "event",
                Line::Log(_) => "log",
            };
            assert_eq!(got, want, "line: {line}");
            n += 1;
        }
        assert!(n >= 8, "expected the vector file to hold cases, found {n}");
    }
}
