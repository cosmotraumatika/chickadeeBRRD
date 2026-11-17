// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

//! Request/response exchange over any byte stream.
//!
//! Generic over `Read + Write` so the same code drives a real serial port and
//! the scripted fakes in the tests.

use std::io::{self, Read, Write};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use chickadee_proto::{classify, Command, Line, Response, MAX_LINE_LEN};
use serde_json::{Map, Value};

#[derive(Debug)]
pub enum LinkError {
    /// No matching response arrived in time.
    Timeout,
    /// The port failed underneath us.
    Io(io::Error),
    /// The stick answered with `ok: false`.
    Device { err: String, msg: Option<String> },
}

impl From<io::Error> for LinkError {
    fn from(e: io::Error) -> Self {
        LinkError::Io(e)
    }
}

pub struct Link<T> {
    io: T,
    pending: Vec<u8>,
    next_id: u32,
}

impl<T: Read + Write> Link<T> {
    /// Start ids at a time-derived value so a response left over from an
    /// earlier session is unlikely to match a new request.
    pub fn new(io: T) -> Self {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.subsec_nanos()).unwrap_or(0);
        Self::with_start_id(io, nanos % 1_000_000 + 1)
    }

    pub fn with_start_id(io: T, start_id: u32) -> Self {
        Self { io, pending: Vec::new(), next_id: start_id }
    }

    /// Next complete line from the stick, without its line ending.
    /// Returns `Ok(None)` when the deadline passes first; `None` waits forever.
    pub fn next_line(&mut self, deadline: Option<Instant>) -> io::Result<Option<String>> {
        let mut chunk = [0u8; 256];
        loop {
            if let Some(pos) = self.pending.iter().position(|b| *b == b'\n') {
                let line: Vec<u8> = self.pending.drain(..=pos).collect();
                let text = String::from_utf8_lossy(&line[..line.len() - 1]);
                return Ok(Some(text.trim_end_matches('\r').to_string()));
            }
            if self.pending.len() > 4 * MAX_LINE_LEN {
                // A runaway line with no newline: drop it and resynchronise.
                self.pending.clear();
            }
            if deadline.is_some_and(|d| Instant::now() >= d) {
                return Ok(None);
            }
            match self.io.read(&mut chunk) {
                Ok(0) => std::thread::sleep(Duration::from_millis(5)),
                Ok(n) => self.pending.extend_from_slice(&chunk[..n]),
                Err(e)
                    if matches!(
                        e.kind(),
                        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) => {}
                Err(e) => return Err(e),
            }
        }
    }

    /// Send a typed command and wait for its response.
    pub fn request(
        &mut self,
        cmd: &Command,
        timeout: Duration,
        on_other: &mut dyn FnMut(&Line),
    ) -> Result<Response, LinkError> {
        let Value::Object(obj) = serde_json::to_value(cmd).expect("command serializes") else {
            unreachable!("commands serialize to objects")
        };
        self.exchange(obj, timeout, on_other)
    }

    /// Send any JSON object as a request and wait for its response.
    /// An `id` is assigned unless the object already carries a numeric one.
    /// Lines that are not the matching response go to `on_other`.
    pub fn exchange(
        &mut self,
        mut obj: Map<String, Value>,
        timeout: Duration,
        on_other: &mut dyn FnMut(&Line),
    ) -> Result<Response, LinkError> {
        let id = match obj.get("id").and_then(Value::as_u64) {
            Some(id) => id as u32,
            None => {
                let id = self.next_id;
                self.next_id = self.next_id.wrapping_add(1).max(1);
                obj.insert("id".into(), Value::from(id));
                id
            }
        };
        let mut line = serde_json::to_string(&obj).expect("object serializes");
        line.push('\n');
        self.io.write_all(line.as_bytes())?;
        self.io.flush()?;

        let deadline = Instant::now() + timeout;
        loop {
            let Some(text) = self.next_line(Some(deadline))? else {
                return Err(LinkError::Timeout);
            };
            match classify(&text) {
                Line::Response(resp) if resp.id == id => {
                    return if resp.ok {
                        Ok(resp)
                    } else {
                        Err(LinkError::Device {
                            err: resp.err.unwrap_or_else(|| "error".into()),
                            msg: resp.msg,
                        })
                    };
                }
                other => on_other(&other),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    /// A scripted stick: hands out queued bytes a few at a time, records writes.
    struct Fake {
        rx: VecDeque<u8>,
        tx: Vec<u8>,
        chunk: usize,
    }

    impl Fake {
        fn sending(text: &str) -> Self {
            Self { rx: text.bytes().collect(), tx: Vec::new(), chunk: 7 }
        }
    }

    impl Read for Fake {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            if self.rx.is_empty() {
                return Err(io::ErrorKind::TimedOut.into());
            }
            let n = self.chunk.min(buf.len()).min(self.rx.len());
            for slot in buf.iter_mut().take(n) {
                *slot = self.rx.pop_front().unwrap();
            }
            Ok(n)
        }
    }

    impl Write for Fake {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.tx.extend_from_slice(buf);
            Ok(buf.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    const SHORT: Duration = Duration::from_millis(50);

    #[test]
    fn finds_its_response_among_logs_events_and_stale_replies() {
        let script = "ESP-ROM:esp32c5-eco2-20250121\r\n\
                      {\"ev\":\"boot\",\"fw\":\"0.1.0\",\"proto\":0}\n\
                      {\"id\":99,\"ok\":true,\"state\":\"stale\"}\n\
                      {\"id\":1,\"ok\":true,\"state\":\"idle\",\"uptime_ms\":5}\n";
        let mut link = Link::with_start_id(Fake::sending(script), 1);
        let mut others = Vec::new();
        let resp = link.request(&Command::Status, SHORT, &mut |l| others.push(l.clone())).unwrap();
        assert_eq!(resp.data["state"], "idle");
        assert_eq!(others.len(), 3);
        assert!(matches!(others[0], Line::Log(ref s) if s == "ESP-ROM:esp32c5-eco2-20250121"));
        assert!(matches!(others[1], Line::Event(_)));
        assert!(matches!(others[2], Line::Response(ref r) if r.id == 99));
        assert_eq!(String::from_utf8(link.io.tx.clone()).unwrap(), "{\"cmd\":\"status\",\"id\":1}\n");
    }

    #[test]
    fn times_out_when_the_stick_stays_silent() {
        let mut link = Link::with_start_id(Fake::sending("I (1) boot: nothing useful\n"), 1);
        let err = link.request(&Command::Hello, SHORT, &mut |_| {}).unwrap_err();
        assert!(matches!(err, LinkError::Timeout));
    }

    #[test]
    fn device_error_surfaces_code_and_message() {
        let script = "{\"id\":1,\"ok\":false,\"err\":\"not_implemented\",\"msg\":\"display driver pending\"}\n";
        let mut link = Link::with_start_id(Fake::sending(script), 1);
        let err = link
            .request(&Command::DisplayText { text: "hi".into() }, SHORT, &mut |_| {})
            .unwrap_err();
        match err {
            LinkError::Device { err, msg } => {
                assert_eq!(err, "not_implemented");
                assert_eq!(msg.as_deref(), Some("display driver pending"));
            }
            other => panic!("unexpected: {other:?}"),
        }
    }

    #[test]
    fn caller_supplied_id_is_respected() {
        let mut link = Link::with_start_id(Fake::sending("{\"id\":500,\"ok\":true}\n"), 1);
        let mut obj = Map::new();
        obj.insert("cmd".into(), "status".into());
        obj.insert("id".into(), 500.into());
        assert!(link.exchange(obj, SHORT, &mut |_| {}).unwrap().ok);
    }

    #[test]
    fn ids_advance_between_requests() {
        let script = "{\"id\":1,\"ok\":true}\n{\"id\":2,\"ok\":true}\n";
        let mut link = Link::with_start_id(Fake::sending(script), 1);
        link.request(&Command::Status, SHORT, &mut |_| {}).unwrap();
        link.request(&Command::Status, SHORT, &mut |_| {}).unwrap();
        let sent = String::from_utf8(link.io.tx.clone()).unwrap();
        assert!(sent.contains("\"id\":1") && sent.contains("\"id\":2"));
    }
}
