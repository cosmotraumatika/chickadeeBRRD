// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

//! chickchirp: talk to a chickadeeBRRD stick over USB serial.

mod discover;
mod link;

use std::io::{self, IsTerminal, Write};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use chickadee_proto::{classify, Command, Hello, Lease, Line, Status, PROTO_VERSION};
use clap::{CommandFactory, Parser, Subcommand};
use clap_complete::Shell;
use serde_json::{Map, Value};

use link::{Link, LinkError};

// Exit codes. 2 is clap's own code for usage errors.
const EXIT_DEVICE_ERROR: u8 = 1;
const EXIT_NO_DEVICE: u8 = 3;
const EXIT_TIMEOUT: u8 = 4;
const EXIT_IO: u8 = 5;

#[derive(Parser)]
#[command(
    name = "chickchirp",
    version,
    about = "Talk to a chickadeeBRRD stick over USB serial",
    after_help = "Exit codes: 0 ok, 1 the stick refused the command, 2 usage error,\n\
                  3 no stick (or more than one) found, 4 no response in time, 5 port I/O error."
)]
struct Cli {
    /// Serial port, e.g. /dev/ttyACM0 or COM3. Auto-detected when exactly one stick is attached.
    #[arg(short, long, global = true, env = "CHICKCHIRP_PORT", value_name = "PORT")]
    port: Option<String>,

    /// How long to wait for the stick to answer, e.g. 2s, 500ms.
    #[arg(long, global = true, default_value = "2s", value_parser = parse_duration, value_name = "DURATION")]
    timeout: Duration,

    /// Print machine-readable JSON on stdout.
    #[arg(long, global = true)]
    json: bool,

    /// Show the stick's log lines and events on stderr while waiting.
    #[arg(short, long, global = true)]
    verbose: bool,

    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// List attached sticks
    Devices {
        /// List every serial port, not only sticks
        #[arg(long)]
        all: bool,
    },
    /// Show firmware, protocol version, board and capabilities
    Info,
    /// Show what the stick is doing now
    Status,
    /// Show a short message on the stick's screen
    Display {
        /// The text to show
        text: String,
    },
    /// Run a transmit session in the foreground until Ctrl-C
    Start {
        /// Signal type to send; `chickchirp info` lists what the stick offers
        #[arg(long, value_name = "SIGNAL")]
        signal: String,
        /// Stop after this long, e.g. 60s. The stick's own limit still applies
        #[arg(long, value_parser = parse_duration, value_name = "DURATION")]
        duration: Option<Duration>,
    },
    /// End a running session
    Stop,
    /// Print everything the stick sends until interrupted
    Monitor,
    /// Send one JSON request as written and print the response
    Raw {
        /// A JSON object such as '{"cmd":"status"}'; an id is added if absent
        line: String,
    },
    /// Generate a shell completion script
    Completions { shell: Shell },
}

/// An error on its way out: the message for stderr and the exit code.
struct Failure {
    code: u8,
    message: String,
}

fn fail<T>(code: u8, message: impl Into<String>) -> Result<T, Failure> {
    Err(Failure { code, message: message.into() })
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    match run(cli) {
        Ok(()) => ExitCode::SUCCESS,
        Err(f) => {
            eprintln!("chickchirp: {}", f.message);
            ExitCode::from(f.code)
        }
    }
}

fn run(cli: Cli) -> Result<(), Failure> {
    match &cli.cmd {
        Cmd::Completions { shell } => {
            clap_complete::generate(*shell, &mut Cli::command(), "chickchirp", &mut io::stdout());
            Ok(())
        }
        Cmd::Devices { all } => devices(*all, cli.json),
        Cmd::Info => {
            let (port, mut link) = open(&cli)?;
            let resp = ask(&mut link, &Command::Hello, &cli)?;
            let hello: Hello = resp
                .payload()
                .or_else(|e| fail(EXIT_DEVICE_ERROR, format!("the stick's hello reply is malformed: {e}")))?;
            if hello.proto != PROTO_VERSION {
                eprintln!(
                    "chickchirp: warning: stick speaks protocol {}, this tool speaks {}",
                    hello.proto, PROTO_VERSION
                );
            }
            if cli.json {
                let mut data = resp.data;
                data.insert("port".into(), Value::from(port));
                print_json(&data);
            } else {
                let caps = if hello.caps.is_empty() { "(none)".to_string() } else { hello.caps.join(", ") };
                println!("device    {}", hello.device);
                println!("board     {}", hello.board);
                println!("firmware  {}", hello.fw);
                println!("protocol  {}", hello.proto);
                println!("caps      {caps}");
                if !hello.signals.is_empty() {
                    println!("signals   {}", hello.signals.join(", "));
                }
                if hello.limits.lease_ms > 0 {
                    println!(
                        "limits    lease {}, session {}",
                        human_duration(hello.limits.lease_ms),
                        human_duration(hello.limits.session_max_ms)
                    );
                }
                println!("port      {port}");
            }
            Ok(())
        }
        Cmd::Status => {
            let (_, mut link) = open(&cli)?;
            let resp = ask(&mut link, &Command::Status, &cli)?;
            if cli.json {
                print_json(&resp.data);
            } else {
                let status: Status = resp
                    .payload()
                    .or_else(|e| fail(EXIT_DEVICE_ERROR, format!("the stick's status reply is malformed: {e}")))?;
                if status.state == "transmitting" {
                    println!(
                        "transmitting {}  {} left",
                        status.signal.as_deref().unwrap_or("?"),
                        human_duration(status.session_remaining_ms.unwrap_or(0))
                    );
                } else {
                    let last = match status.last_stop.as_deref() {
                        Some(reason) => format!("  last stop: {}", describe_stop(reason)),
                        None => String::new(),
                    };
                    println!("{}  up {}{last}", status.state, human_duration(status.uptime_ms));
                }
            }
            Ok(())
        }
        Cmd::Start { signal, duration } => start(&cli, signal, *duration),
        Cmd::Stop => {
            let (_, mut link) = open(&cli)?;
            let resp = ask(&mut link, &Command::TxStop, &cli)?;
            if cli.json {
                print_json(&resp.data);
            } else if resp.data.get("stopped").and_then(Value::as_bool) == Some(true) {
                println!("stopped");
            } else {
                println!("already idle");
            }
            Ok(())
        }
        Cmd::Display { text } => {
            let (_, mut link) = open(&cli)?;
            let resp = ask(&mut link, &Command::DisplayText { text: text.clone() }, &cli)?;
            if cli.json {
                print_json(&resp.data);
            }
            Ok(())
        }
        Cmd::Raw { line } => {
            let obj = match serde_json::from_str::<Value>(line) {
                Ok(Value::Object(obj)) => obj,
                Ok(_) => return fail(2, "raw: the request must be a JSON object"),
                Err(e) => return fail(2, format!("raw: not valid JSON: {e}")),
            };
            let (_, mut link) = open(&cli)?;
            let verbose = cli.verbose;
            match link.exchange(obj, cli.timeout, &mut |l| echo_other(l, verbose)) {
                Ok(resp) => {
                    println!("{}", serde_json::to_string(&resp).expect("response serializes"));
                    Ok(())
                }
                Err(e) => Err(explain(e, &cli)),
            }
        }
        Cmd::Monitor => {
            let (_, mut link) = open(&cli)?;
            loop {
                let text = match link.next_line(None) {
                    Ok(Some(text)) => text,
                    Ok(None) => continue,
                    Err(e) => return fail(EXIT_IO, format!("lost the port: {e}")),
                };
                let is_log = matches!(classify(&text), Line::Log(_));
                if cli.json && is_log {
                    eprintln!("{text}");
                } else {
                    println!("{text}");
                }
            }
        }
    }
}

/// Run a transmit session: start it, keep renewing the lease, stop on Ctrl-C.
///
/// The stick stops by itself if the renewals cease, so nothing here is relied
/// on for safety; this loop only keeps a wanted session alive.
fn start(cli: &Cli, signal: &str, duration: Option<Duration>) -> Result<(), Failure> {
    let (_, mut link) = open(cli)?;
    let json = cli.json;
    let verbose = cli.verbose;
    let mut seen = |line: &Line| {
        if let (true, Line::Event(ev)) = (json, line) {
            println!("{}", serde_json::to_string(ev).unwrap_or_default());
        }
        echo_other(line, verbose);
    };

    let hello: Hello = link
        .request(&Command::Hello, cli.timeout, &mut seen)
        .map_err(|e| explain(e, cli))?
        .payload()
        .or_else(|e| fail(EXIT_DEVICE_ERROR, format!("the stick's hello reply is malformed: {e}")))?;
    if !hello.caps.iter().any(|c| c == "tx") {
        return fail(EXIT_DEVICE_ERROR, format!("firmware {} has no transmit support", hello.fw));
    }
    if !hello.signals.iter().any(|s| s == signal) {
        return fail(
            EXIT_DEVICE_ERROR,
            format!("the stick does not offer signal '{signal}'; it offers: {}", hello.signals.join(", ")),
        );
    }

    let interrupted = Arc::new(AtomicBool::new(false));
    {
        let flag = interrupted.clone();
        ctrlc::set_handler(move || flag.store(true, Ordering::SeqCst))
            .or_else(|e| fail(EXIT_IO, format!("cannot install the Ctrl-C handler: {e}")))?;
    }

    let lease: Lease = link
        .request(&Command::TxStart { signal: signal.to_string() }, cli.timeout, &mut seen)
        .map_err(|e| explain(e, cli))?
        .payload()
        .or_else(|e| fail(EXIT_DEVICE_ERROR, format!("the stick's tx.start reply is malformed: {e}")))?;

    // Renew three times per lease so one lost renewal does not end the session.
    let renew_every = Duration::from_millis((lease.lease_ms / 3).max(100));
    let began = Instant::now();
    let mut next_renew = began + renew_every;
    let live = !json && io::stderr().is_terminal();
    let progress = |remaining_ms: u64| {
        if live {
            eprint!("\rtransmitting {signal}  {} left   ", human_duration(remaining_ms));
        }
    };
    if !json {
        eprintln!("session started; Ctrl-C to stop");
    }
    progress(lease.session_remaining_ms);

    let ended = loop {
        if interrupted.load(Ordering::SeqCst) {
            break "interrupted";
        }
        if duration.is_some_and(|d| began.elapsed() >= d) {
            break "duration";
        }
        let slice = (Instant::now() + Duration::from_millis(100)).min(next_renew);
        match link.next_line(Some(slice)) {
            Ok(Some(text)) => {
                let line = classify(&text);
                seen(&line);
                if let Line::Event(ev) = &line {
                    if ev.ev == "state" && ev.data.get("state").and_then(Value::as_str) == Some("idle") {
                        let reason = ev.data.get("reason").and_then(Value::as_str).unwrap_or("unknown");
                        return ended_by_stick(reason, live);
                    }
                }
            }
            Ok(None) => {}
            Err(e) => return fail(EXIT_IO, format!("lost the port: {e}")),
        }
        if Instant::now() >= next_renew {
            match link.request(&Command::LeaseRenew, cli.timeout, &mut seen) {
                Ok(resp) => {
                    if let Ok(lease) = resp.payload::<Lease>() {
                        progress(lease.session_remaining_ms);
                    }
                }
                Err(LinkError::Device { err, .. }) if err == "not_active" => {
                    // The stick ended the session on its own; ask it why.
                    let reason = link
                        .request(&Command::Status, cli.timeout, &mut seen)
                        .ok()
                        .and_then(|r| r.payload::<Status>().ok())
                        .and_then(|s| s.last_stop)
                        .unwrap_or_else(|| "unknown".into());
                    return ended_by_stick(&reason, live);
                }
                Err(e) => return Err(explain(e, cli)),
            }
            next_renew = Instant::now() + renew_every;
        }
    };

    link.request(&Command::TxStop, cli.timeout, &mut seen).map_err(|e| explain(e, cli))?;
    // Give the stick's own "idle" event a moment to arrive so it gets reported.
    let settle = Instant::now() + Duration::from_millis(200);
    while let Ok(Some(text)) = link.next_line(Some(settle)) {
        seen(&classify(&text));
    }
    if live {
        eprintln!();
    }
    if !json {
        let why = if ended == "duration" { "duration reached" } else { "interrupted" };
        println!("stopped after {} ({why})", human_duration(began.elapsed().as_millis() as u64));
    }
    Ok(())
}

/// The stick ended the session itself. Its limit is an expected end; anything
/// else means this tool failed to keep the lease alive.
fn ended_by_stick(reason: &str, live: bool) -> Result<(), Failure> {
    if live {
        eprintln!();
    }
    if reason == "limit" {
        println!("stopped by the stick: {}", describe_stop(reason));
        Ok(())
    } else {
        fail(EXIT_DEVICE_ERROR, format!("the stick ended the session: {}", describe_stop(reason)))
    }
}

fn describe_stop(reason: &str) -> &str {
    match reason {
        "stopped" => "stopped by the host",
        "lease_expired" => "lease expired (the host went quiet)",
        "limit" => "session time limit reached",
        other => other,
    }
}

fn devices(all: bool, json: bool) -> Result<(), Failure> {
    let ports = discover::ports().or_else(|e| fail(EXIT_IO, format!("could not list serial ports: {e}")))?;
    let shown: Vec<_> = ports.iter().filter(|p| all || p.is_stick).collect();
    if json {
        let rows: Vec<Value> = shown
            .iter()
            .map(|p| {
                serde_json::json!({
                    "port": p.port,
                    "usb": p.vid_pid.map(|(v, d)| format!("{v:04x}:{d:04x}")),
                    "serial": p.serial,
                    "product": p.product,
                    "stick": p.is_stick,
                })
            })
            .collect();
        println!("{}", Value::Array(rows));
        return Ok(());
    }
    if shown.is_empty() {
        eprintln!("chickchirp: no {} found", if all { "serial ports" } else { "sticks" });
        return Ok(());
    }
    for p in shown {
        let usb = p.vid_pid.map(|(v, d)| format!("{v:04x}:{d:04x}")).unwrap_or_else(|| "-".into());
        let serial = p.serial.as_deref().unwrap_or("-");
        let product = p.product.as_deref().unwrap_or("");
        println!("{}\t{usb}\t{serial}\t{product}", p.port);
    }
    Ok(())
}

type Port = Box<dyn serialport::SerialPort>;

/// Resolve the port (flag, environment, or auto-detect) and open it.
fn open(cli: &Cli) -> Result<(String, Link<Port>), Failure> {
    let path = match &cli.port {
        Some(p) => p.clone(),
        None => {
            let ports = discover::ports().or_else(|e| fail(EXIT_IO, format!("could not list serial ports: {e}")))?;
            let sticks: Vec<_> = ports.into_iter().filter(|p| p.is_stick).collect();
            match sticks.as_slice() {
                [one] => one.port.clone(),
                [] => {
                    return fail(
                        EXIT_NO_DEVICE,
                        "no stick found. Plug it in (under WSL, attach it with usbipd first), \
                         or name the port with --port",
                    )
                }
                many => {
                    let names: Vec<_> = many.iter().map(|p| p.port.as_str()).collect();
                    return fail(
                        EXIT_NO_DEVICE,
                        format!("{} sticks found ({}); pick one with --port", many.len(), names.join(", ")),
                    );
                }
            }
        }
    };
    // The baud rate is ignored by the stick's USB serial but the API wants one.
    let port = serialport::new(&path, 115_200)
        .timeout(Duration::from_millis(50))
        .open()
        .or_else(|e| {
            let hint = match e.kind() {
                _ if e.description.contains("exclusive lock") => {
                    ". Another program has the port open (a running `chickchirp start`?)"
                }
                serialport::ErrorKind::Io(io::ErrorKind::PermissionDenied) => {
                    ". Your user needs to be in the group that owns the port (see `ls -l` on it)"
                }
                serialport::ErrorKind::NoDevice | serialport::ErrorKind::Io(io::ErrorKind::NotFound) => {
                    ". Is the stick attached?"
                }
                _ => "",
            };
            fail(EXIT_IO, format!("cannot open {path}: {e}{hint}"))
        })?;
    Ok((path, Link::new(port)))
}

fn ask(link: &mut Link<Port>, cmd: &Command, cli: &Cli) -> Result<chickadee_proto::Response, Failure> {
    let verbose = cli.verbose;
    link.request(cmd, cli.timeout, &mut |l| echo_other(l, verbose)).map_err(|e| explain(e, cli))
}

fn echo_other(line: &Line, verbose: bool) {
    if !verbose {
        return;
    }
    match line {
        Line::Log(text) => eprintln!("log: {text}"),
        Line::Event(ev) => eprintln!("event: {}", serde_json::to_string(ev).unwrap_or_default()),
        Line::Response(r) => eprintln!("stray response: {}", serde_json::to_string(r).unwrap_or_default()),
    }
}

fn explain(err: LinkError, cli: &Cli) -> Failure {
    match err {
        LinkError::Timeout => Failure {
            code: EXIT_TIMEOUT,
            message: format!(
                "no response within {}. Is chickadeeBRRD firmware running on the stick? \
                 Try --verbose to see what it is sending",
                human_duration(cli.timeout.as_millis() as u64)
            ),
        },
        LinkError::Io(e) => Failure { code: EXIT_IO, message: format!("port error: {e}") },
        LinkError::Device { err, msg } => Failure {
            code: EXIT_DEVICE_ERROR,
            message: match msg {
                Some(msg) => format!("the stick refused: {err} ({msg})"),
                None => format!("the stick refused: {err}"),
            },
        },
    }
}

fn print_json(data: &Map<String, Value>) {
    let mut out = io::stdout().lock();
    let _ = writeln!(out, "{}", Value::Object(data.clone()));
}

fn parse_duration(s: &str) -> Result<Duration, String> {
    let s = s.trim();
    let (number, scale) = if let Some(n) = s.strip_suffix("ms") {
        (n, 0.001)
    } else if let Some(n) = s.strip_suffix('s') {
        (n, 1.0)
    } else {
        (s, 1.0)
    };
    match number.trim().parse::<f64>() {
        Ok(v) if v.is_finite() && v > 0.0 => Ok(Duration::from_secs_f64(v * scale)),
        _ => Err(format!("'{s}' is not a duration; try 2s or 500ms")),
    }
}

fn human_duration(ms: u64) -> String {
    let secs = ms / 1000;
    match secs {
        0 => format!("{ms}ms"),
        1..=59 => format!("{secs}s"),
        60..=3599 => format!("{}m{:02}s", secs / 60, secs % 60),
        _ => format!("{}h{:02}m", secs / 3600, secs % 3600 / 60),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn durations_parse() {
        assert_eq!(parse_duration("2s").unwrap(), Duration::from_secs(2));
        assert_eq!(parse_duration("500ms").unwrap(), Duration::from_millis(500));
        assert_eq!(parse_duration("1.5").unwrap(), Duration::from_millis(1500));
        assert!(parse_duration("soon").is_err());
        assert!(parse_duration("0s").is_err());
        assert!(parse_duration("-1s").is_err());
    }

    #[test]
    fn durations_print() {
        assert_eq!(human_duration(250), "250ms");
        assert_eq!(human_duration(12_000), "12s");
        assert_eq!(human_duration(192_000), "3m12s");
        assert_eq!(human_duration(3_720_000), "1h02m");
    }

    #[test]
    fn cli_definition_is_consistent() {
        Cli::command().debug_assert();
    }
}
