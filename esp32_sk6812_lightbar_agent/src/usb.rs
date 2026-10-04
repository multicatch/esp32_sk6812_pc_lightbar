use crate::{CONTROLLER_GREETING, ESPRESSIF_VID, LedbarCommand};
use serialport::{Error, SerialPort, SerialPortInfo};
use std::io::{Read, Write};
use std::time::Duration;
use log::{error, info};

pub fn retry_if_closed(port: Result<Box<dyn SerialPort>, Error>) -> Result<Box<dyn SerialPort>, Error> {
    if port.is_ok() {
        port
    } else {
        try_connect_to_com()
    }
}

pub fn try_connect_to_com() -> Result<Box<dyn SerialPort>, Error> {
    let esp32_port = serialport::available_ports()?
        .into_iter()
        .filter(|port| {
            matches!(&port.port_type, serialport::SerialPortType::UsbPort(info)
                if info.vid == ESPRESSIF_VID
            )
        })
        .find_map(|port| {
            let con = serialport::new(port.port_name.clone(), 115_200)
                .timeout(Duration::from_millis(500))
                .open();

            if let Ok(con) = con {
                let result = read_greeting_and_detect_controller(&port, con);
                Some((port, result))
            } else {
                None
            }
        });

    if let Some((port, con)) = esp32_port {
        info!("Connected to esp32: {}", port.port_name);
        Ok(con?)
    } else {
        Err(std::io::Error::other("Cannot find usable USB device").into())
    }
}

fn read_greeting_and_detect_controller(
    port: &SerialPortInfo,
    mut con: Box<dyn SerialPort>,
) -> Result<Box<dyn SerialPort>, std::io::Error> {
    con.write_all(b"H\n")?;
    let mut response = String::new();
    let mut buffer = [0u8; 64];
    loop {
        match con.read(&mut buffer) {
            Ok(n) => {
                response.push_str(&String::from_utf8_lossy(&buffer[..n]));

                if response.trim() == CONTROLLER_GREETING {
                    return Ok(con);
                }
                if response.len() > CONTROLLER_GREETING.len() {
                    error!(
                        "Incorrect greeting from device {}: {}",
                        port.port_name, response
                    );
                    return Err(std::io::Error::other(format!(
                        "USB device is not a compatible LED controller: {}",
                        port.port_name
                    )));
                }
            }
            Err(e) => {
                error!("Error reading from serial port: {}", e);
                return Err(e);
            }
        }
    }
}

pub fn write_if_possible(con: &mut Box<dyn SerialPort>, command: LedbarCommand) -> Result<(), Error> {
    match command {
        LedbarCommand::Wake => {
            con.write_all(b"W\n")?;
        }
        LedbarCommand::Sleep => {
            con.write_all(b"S\n")?;
        }
        LedbarCommand::ShutDown => {
            con.write_all(b"Z\n")?;
        }
    }

    Ok(())
}
