// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

//! Find attached sticks by USB identity.

use chickadee_proto::{USB_PID, USB_VID};
use serialport::{SerialPortInfo, SerialPortType};

pub struct Found {
    pub port: String,
    pub vid_pid: Option<(u16, u16)>,
    pub serial: Option<String>,
    pub product: Option<String>,
    pub is_stick: bool,
}

/// Every serial port the OS reports, flagged when it carries the stick's USB id.
pub fn ports() -> Result<Vec<Found>, serialport::Error> {
    let mut found: Vec<Found> = serialport::available_ports()?.into_iter().map(describe).collect();
    found.sort_by(|a, b| a.port.cmp(&b.port));
    Ok(found)
}

fn describe(info: SerialPortInfo) -> Found {
    match info.port_type {
        SerialPortType::UsbPort(usb) => Found {
            port: info.port_name,
            vid_pid: Some((usb.vid, usb.pid)),
            serial: usb.serial_number,
            product: usb.product,
            is_stick: usb.vid == USB_VID && usb.pid == USB_PID,
        },
        _ => Found { port: info.port_name, vid_pid: None, serial: None, product: None, is_stick: false },
    }
}
