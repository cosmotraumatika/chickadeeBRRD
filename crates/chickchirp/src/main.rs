// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

//! chickchirp: talk to a chickadeeBRRD stick over USB serial.

mod discover;
mod link;

use std::io::{self, IsTerminal, Write};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

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
        /// Time limit for the session, e.g. 90s or 10m. Without it the stick
        /// applies its default (5 minutes); the longest it allows is 60m
        #[arg(long, value_parser = parse_duration, value_name = "DURATION")]
        duration: Option<Duration>,
        /// Latitude to broadcast, in degrees, north positive. Needed by radio signals
        #[arg(long, env = "CHICKCHIRP_LAT", allow_negative_numbers = true, value_name = "DEGREES")]
        lat: Option<f64>,
        /// Longitude to broadcast, in degrees, east positive. Needed by radio signals
        #[arg(long, env = "CHICKCHIRP_LON", allow_negative_numbers = true, value_name = "DEGREES")]
        lon: Option<f64>,
        /// Transmit power in dBm. Without it the stick uses its lowest setting;
        /// `chickchirp info` shows the range
        #[arg(long, allow_negative_numbers = true, value_name = "DBM")]
        power: Option<i32>,
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
                let limits = &hello.limits;
                if limits.lease_ms > 0 && limits.session_default_ms > 0 {
                    println!(
                        "limits    lease {}, session {} unless asked, {} at most",
                        human_duration(limits.lease_ms),
                        human_duration(limits.session_default_ms),
                        human_duration(limits.session_max_ms)
                    );
                } else if limits.lease_ms > 0 {
                    println!(
                        "limits    lease {}, session {}",
                        human_duration(limits.lease_ms),
                        human_duration(limits.session_max_ms)
                    );
                }
                if let (Some(default), Some(max)) = (limits.power_default_dbm, limits.power_max_dbm) {
                    println!("power     {default} dBm unless asked, {max} dBm at most");
                }
                if let Some(rid) = &hello.rid {
                    println!("identity  {}  \"{}\"", rid.id, rid.self_id);
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
                    let power = status.power_dbm.map(|p| format!(" at {p} dBm")).unwrap_or_default();
                    let position = match (status.lat, status.lon) {
                        (Some(lat), Some(lon)) => format!("  position {lat}, {lon}"),
                        _ => String::new(),
                    };
                    println!(
                        "transmitting {}{power}  {} left{position}",
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
        Cmd::Start { signal, duration, lat, lon, power } => {
            start(&cli, &StartOptions { signal, duration: *duration, lat: *lat, lon: *lon, power: *power })
        }
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

/// What `chickchirp start` was asked for.
struct StartOptions<'a> {
    signal: &'a str,
    duration: Option<Duration>,
    lat: Option<f64>,
    lon: Option<f64>,
    power: Option<i32>,
}

/// The LED stand-in is the one signal that is not a radio transmission.
fn is_radio(signal: &str) -> bool {
    signal != "led"
}

/// The host clock as Unix seconds, to the millisecond.
fn unix_now() -> f64 {
    let since = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    since.as_millis() as f64 / 1000.0
}

/// Check what was asked for against what the stick says it allows, so a
/// mistake is reported here in plain words. The stick checks again itself.
fn check_start(opts: &StartOptions, hello: &Hello) -> Result<(), Failure> {
    let signal = opts.signal;
    if is_radio(signal) {
        let (Some(lat), Some(lon)) = (opts.lat, opts.lon) else {
            return fail(
                2,
                format!(
                    "signal '{signal}' broadcasts a position: give --lat and --lon \
                     (or set CHICKCHIRP_LAT and CHICKCHIRP_LON)"
                ),
            );
        };
        if !(-90.0..=90.0).contains(&lat) || !(-180.0..=180.0).contains(&lon) {
            return fail(2, "--lat must be within -90..90 and --lon within -180..180");
        }
        if lat == 0.0 && lon == 0.0 {
            return fail(2, "0, 0 means \"position unknown\" in Remote ID; give the real position");
        }
    } else if opts.power.is_some() {
        return fail(2, format!("signal '{signal}' has no power setting"));
    }
    if let Some(power) = opts.power {
        let limits = &hello.limits;
        if let Some(max) = limits.power_max_dbm {
            if power > max {
                return fail(2, format!("--power {power} is above this firmware's ceiling of {max} dBm"));
            }
        }
        if let (Some(min), Some(max), Some(step)) = (limits.power_min_dbm, limits.power_max_dbm, limits.power_step_db) {
            if step > 0 && (power < min || (power - min) % step != 0) {
                let settings: Vec<String> = (min..=max).step_by(step as usize).map(|p| p.to_string()).collect();
                return fail(
                    2,
                    format!("--power {power} is not a setting the radio has; choose from {}", settings.join(", ")),
                );
            }
        }
    }
    if let Some(duration) = opts.duration {
        let max = hello.limits.session_max_ms;
        if max > 0 && duration.as_millis() > u128::from(max) {
            return fail(
                2,
                format!(
                    "--duration {} is longer than the stick allows ({})",
                    human_duration(duration.as_millis() as u64),
                    human_duration(max)
                ),
            );
        }
    }
    Ok(())
}

/// Run a transmit session: start it, keep renewing the lease, stop on Ctrl-C.
///
/// The stick stops by itself if the renewals cease, so nothing here is relied
/// on for safety; this loop only keeps a wanted session alive.
fn start(cli: &Cli, opts: &StartOptions) -> Result<(), Failure> {
    let signal = opts.signal;
    let duration = opts.duration;
    let radio = is_radio(signal);
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
    check_start(opts, &hello)?;

    let interrupted = Arc::new(AtomicBool::new(false));
    {
        let flag = interrupted.clone();
        ctrlc::set_handler(move || flag.store(true, Ordering::SeqCst))
            .or_else(|e| fail(EXIT_IO, format!("cannot install the Ctrl-C handler: {e}")))?;
    }

    let request = Command::TxStart {
        signal: signal.to_string(),
        lat: opts.lat.filter(|_| radio),
        lon: opts.lon.filter(|_| radio),
        time: radio.then(unix_now),
        power_dbm: opts.power,
        max_ms: duration.map(|d| d.as_millis() as u64),
    };
    // Timed from before the request, so this clock never runs behind the stick's.
    let began = Instant::now();
    let lease: Lease = link
        .request(&request, cli.timeout, &mut seen)
        .map_err(|e| explain(e, cli))?
        .payload()
        .or_else(|e| fail(EXIT_DEVICE_ERROR, format!("the stick's tx.start reply is malformed: {e}")))?;

    // Renew three times per lease so one lost renewal does not end the session.
    let renew_every = Duration::from_millis((lease.lease_ms / 3).max(100));
    let mut next_renew = Instant::now() + renew_every;
    let live = !json && io::stderr().is_terminal();
    let power = lease.power_dbm.map(|p| format!("  {p} dBm")).unwrap_or_default();
    let progress = |remaining_ms: u64| {
        if live {
            eprint!("\rtransmitting {signal}{power}  {} left   ", human_duration(remaining_ms));
        }
    };
    if !json {
        if radio {
            // Say plainly what is going on the air, from what the stick reported.
            eprintln!("on the air: Remote ID test messages ({signal})");
            if let Some(rid) = &hello.rid {
                eprintln!("  identity  {}  \"{}\"", rid.id, rid.self_id);
            }
            if let (Some(lat), Some(lon)) = (opts.lat, opts.lon) {
                eprintln!("  position  {lat}, {lon}");
            }
            if let Some(p) = lease.power_dbm {
                eprintln!("  power     {p} dBm");
            }
            // To the nearest second: a moment has already passed since the start.
            let limit_ms = (lease.session_remaining_ms + 500) / 1000 * 1000;
            eprintln!("  limit     {}", human_duration(limit_ms));
        }
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
                        return ended_by_stick(reason, live, json, duration.is_some(), began);
                    }
                }
            }
            Ok(None) => {}
            Err(e) => return fail(EXIT_IO, format!("lost the port: {e}")),
        }
        if Instant::now() >= next_renew {
            match link.request(&Command::LeaseRenew { time: radio.then(unix_now) }, cli.timeout, &mut seen) {
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
                    return ended_by_stick(&reason, live, json, duration.is_some(), began);
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

/// The stick ended the session itself. Its time limit is an expected end;
/// anything else means the session did not run as asked.
fn ended_by_stick(reason: &str, live: bool, json: bool, timed: bool, began: Instant) -> Result<(), Failure> {
    if live {
        eprintln!();
    }
    if reason != "limit" {
        return fail(EXIT_DEVICE_ERROR, format!("the stick ended the session: {}", describe_stop(reason)));
    }
    if json {
        // The idle event has already been printed.
    } else if timed {
        // The limit was the --duration that was asked for.
        println!("stopped after {} (duration reached)", human_duration(began.elapsed().as_millis() as u64));
    } else {
        println!("stopped by the stick: {}", describe_stop(reason));
    }
    Ok(())
}

fn describe_stop(reason: &str) -> &str {
    match reason {
        "stopped" => "stopped by the host",
        "lease_expired" => "lease expired (the host went quiet)",
        "limit" => "session time limit reached",
        "error" => "the radio could not transmit",
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
    let port = serialport::new(&path, 115_200).timeout(Duration::from_millis(50)).open().or_else(|e| {
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
    } else if let Some(n) = s.strip_suffix('m') {
        (n, 60.0)
    } else if let Some(n) = s.strip_suffix('h') {
        (n, 3600.0)
    } else {
        (s, 1.0)
    };
    match number.trim().parse::<f64>() {
        Ok(v) if v.is_finite() && v > 0.0 && v * scale < 1e9 => Ok(Duration::from_secs_f64(v * scale)),
        _ => Err(format!("'{s}' is not a duration; try 500ms, 90s, 10m or 1h")),
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
        assert_eq!(parse_duration("10m").unwrap(), Duration::from_secs(600));
        assert_eq!(parse_duration("1h").unwrap(), Duration::from_secs(3600));
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

    fn hello_with_radio() -> Hello {
        serde_json::from_str(
            r#"{"proto":0,"caps":["tx"],"signals":["led","bt4"],"limits":{"lease_ms":3000,
            "session_default_ms":300000,"session_max_ms":3600000,"power_min_dbm":-24,
            "power_default_dbm":-24,"power_max_dbm":0,"power_step_db":3}}"#,
        )
        .unwrap()
    }

    fn opts(signal: &str) -> StartOptions<'_> {
        StartOptions { signal, duration: None, lat: None, lon: None, power: None }
    }

    #[test]
    fn a_radio_signal_needs_a_real_position() {
        let hello = hello_with_radio();
        assert!(check_start(&opts("led"), &hello).is_ok());
        assert_eq!(check_start(&opts("bt4"), &hello).unwrap_err().code, 2);
        let at = |lat, lon| StartOptions { lat: Some(lat), lon: Some(lon), ..opts("bt4") };
        assert!(check_start(&at(40.9, -95.6), &hello).is_ok());
        assert!(check_start(&at(0.0, 0.0), &hello).is_err());
        assert!(check_start(&at(91.0, 10.0), &hello).is_err());
        assert!(check_start(&at(10.0, -181.0), &hello).is_err());
    }

    #[test]
    fn power_and_duration_are_held_to_the_sticks_limits() {
        let hello = hello_with_radio();
        let with = |power, duration| StartOptions { lat: Some(40.9), lon: Some(-95.6), power, duration, ..opts("bt4") };
        assert!(check_start(&with(Some(-24), None), &hello).is_ok());
        assert!(check_start(&with(Some(0), None), &hello).is_ok());
        assert!(check_start(&with(Some(3), None), &hello).is_err());
        assert!(check_start(&with(Some(-23), None), &hello).is_err());
        assert!(check_start(&with(Some(-27), None), &hello).is_err());
        assert!(check_start(&with(None, Some(Duration::from_secs(3600))), &hello).is_ok());
        assert!(check_start(&with(None, Some(Duration::from_secs(3601))), &hello).is_err());
        // The LED has no power to set.
        assert!(check_start(&StartOptions { power: Some(-24), ..opts("led") }, &hello).is_err());
    }

    #[test]
    fn negative_coordinates_and_power_parse() {
        let cli = Cli::try_parse_from([
            "chickchirp",
            "start",
            "--signal",
            "bt4",
            "--lat",
            "-33.9",
            "--lon",
            "-95.6",
            "--power",
            "-6",
            "--duration",
            "10m",
        ])
        .unwrap();
        let Cmd::Start { lat, lon, power, duration, .. } = cli.cmd else { panic!("not start") };
        assert_eq!((lat, lon, power), (Some(-33.9), Some(-95.6), Some(-6)));
        assert_eq!(duration, Some(Duration::from_secs(600)));
    }

    #[test]
    fn cli_definition_is_consistent() {
        Cli::command().debug_assert();
    }
}
