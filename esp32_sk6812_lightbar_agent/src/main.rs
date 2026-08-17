#![windows_subsystem = "windows"]

use crate::LedbarCommand::{ShutDown, Sleep, Wake};
use serialport::{Error, SerialPort, SerialPortInfo};
use std::io::{Read, Write};
use std::sync::mpsc;
use std::sync::mpsc::{Receiver, Sender};
use std::thread;
use std::thread::sleep;
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DispatchMessageW, GWLP_USERDATA, GetMessageW,
    GetWindowLongPtrW, MSG, PBT_APMRESUMEAUTOMATIC, PBT_APMSUSPEND, RegisterClassW,
    SetWindowLongPtrW, WINDOW_EX_STYLE, WM_ENDSESSION, WM_NCCREATE, WM_POWERBROADCAST, WNDCLASSW,
};
use windows::core::PCWSTR;

const CLASS_NAME: PCWSTR = windows::core::w!("SK6812LightbarAgent");

const ESPRESSIF_VID: u16 = 0x303A;

const CONTROLLER_GREETING: &str = "ESP32-SK6812-LIGHTBAR-V1";

const STALE_MESSAGE_TIME: Duration = Duration::from_secs(5);
const SLEEP_CMD_QUIET_TIME: Duration = Duration::from_secs(10);

#[derive(Debug, Clone)]
struct LedbarCommandMessage(Instant, LedbarCommand);

impl LedbarCommandMessage {
    pub fn new(cmd: LedbarCommand) -> Self {
        LedbarCommandMessage(Instant::now(), cmd)
    }
}

#[derive(Debug, PartialEq, Clone)]
enum LedbarCommand {
    Wake,
    Sleep,
    ShutDown,
}

pub struct Broadcast<T: Clone> {
    tx: Receiver<T>,
    subscribers: Vec<Sender<T>>,
}

impl<T: Clone> Broadcast<T> {
    pub fn new() -> (Sender<T>, Broadcast<T>) {
        let (tx, rx) = mpsc::channel();
        (
            tx,
            Broadcast {
                tx: rx,
                subscribers: Vec::new(),
            },
        )
    }

    pub fn create_subscriber(&mut self) -> Receiver<T> {
        let (tx, rx) = mpsc::channel();
        self.subscribers.push(tx);
        rx
    }

    pub fn run_broadcast(&mut self) {
        loop {
            match self.tx.recv() {
                Ok(data) => {
                    let mut i = 0;
                    while i < self.subscribers.len() {
                        let result = self.subscribers[i].send(data.clone());
                        if result.is_err() {
                            // this channel was closed
                            self.subscribers.remove(i);
                        } else {
                            i += 1;
                        }
                    }
                }
                Err(_) => {
                    // channel disconnected, we propagate it to the subscribers
                    self.subscribers.clear();
                    return;
                }
            }
        }
    }
}

unsafe extern "system" fn wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    unsafe {
        if msg == WM_NCCREATE {
            let create_struct = &*(lparam.0 as *const CREATESTRUCTW);
            let tx_ptr = create_struct.lpCreateParams as *mut Sender<LedbarCommandMessage>;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, tx_ptr as isize);
            LRESULT(1)
        } else if msg == WM_POWERBROADCAST {
            let tx_ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const Sender<LedbarCommandMessage>;
            if !tx_ptr.is_null() {
                let event = wparam.0 as u32;
                if event == PBT_APMSUSPEND {
                    (&*tx_ptr).send(LedbarCommandMessage::new(Sleep)).unwrap();
                } else if event == PBT_APMRESUMEAUTOMATIC {
                    (&*tx_ptr).send(LedbarCommandMessage::new(Wake)).unwrap();
                }
            }
            LRESULT(1)
        } else if msg == WM_ENDSESSION {
            let tx_ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const Sender<LedbarCommandMessage>;
            if !tx_ptr.is_null() && wparam.0 != 0 {
                (&*tx_ptr).send(LedbarCommandMessage::new(ShutDown)).unwrap();
            }
            LRESULT(0)
        } else {
            DefWindowProcW(hwnd, msg, wparam, lparam)
        }
    }
}

fn main() -> windows::core::Result<()> {
    let (tx, mut broadcast) = Broadcast::<LedbarCommandMessage>::new();
    let esp32_rx = broadcast.create_subscriber();
    let ping_rx = broadcast.create_subscriber();

    thread::spawn(move || {
        broadcast.run_broadcast();
    });

    create_hidden_window(&tx)?;

    thread::spawn(move || {
        let mut port = try_connect_to_com();
        let mut last_sleep = Instant::now();

        while let Ok(LedbarCommandMessage(time, command)) = esp32_rx.recv() {
            if time.elapsed() > STALE_MESSAGE_TIME {
                // stale message
                continue;
            }
            if command == Wake && last_sleep.elapsed() < SLEEP_CMD_QUIET_TIME {
                continue; // some background tasks may wake us up too early
            }

            println!("Received command {:?}", command);
            port = retry_if_closed(port);
            match port {
                Ok(ref mut con) => {
                    if command == Sleep {
                        last_sleep = Instant::now();
                    }
                    if let Err(e) = write_if_possible(con, command) {
                        eprintln!("Cannot write command! {}", e);
                    }
                }
                Err(ref err) => {
                    eprintln!("Cannot open port! {}", err.clone());
                }
            }
        }
    });

    tx.send(LedbarCommandMessage::new(Wake)).unwrap(); // first heartbeat

    thread::spawn(move || {
        loop {
            sleep(Duration::from_secs(2));
            if let Ok(LedbarCommandMessage(time, cmd)) = ping_rx.try_recv()
                && cmd != Wake
                && time.elapsed() <= STALE_MESSAGE_TIME
            {
                // slow down, we're either going to sleep or user is trying to shut down the PC
                sleep(SLEEP_CMD_QUIET_TIME);
            }
            tx.send(LedbarCommandMessage::new(Wake)).unwrap();
        }
    });

    unsafe {
        let mut msg = MSG::default();
        while GetMessageW(&mut msg, None, 0, 0).as_bool() {
            DispatchMessageW(&msg);
        }
    }

    Ok(())
}

fn create_hidden_window(tx: &Sender<LedbarCommandMessage>) -> windows::core::Result<()> {
    let tx_box = Box::new(tx.clone());
    let tx_ptr = Box::into_raw(tx_box);

    unsafe {
        let instance = HINSTANCE(GetModuleHandleW(None)?.0);

        let window_class = WNDCLASSW {
            lpfnWndProc: Some(wnd_proc),
            hInstance: instance,
            lpszClassName: CLASS_NAME,
            ..Default::default()
        };

        RegisterClassW(&window_class);

        let hwnd = CreateWindowExW(
            WINDOW_EX_STYLE::default(),
            CLASS_NAME,
            windows::core::w!("SK6812 Lightbar Controller Agent"),
            Default::default(),
            0,
            0,
            0,
            0,
            None,
            None,
            Some(instance),
            Some(tx_ptr.cast()),
        );

        if hwnd?.is_invalid() {
            panic!("CreateWindowExW failed.");
        }
    };
    Ok(())
}

fn retry_if_closed(port: Result<Box<dyn SerialPort>, Error>) -> Result<Box<dyn SerialPort>, Error> {
    if port.is_ok() {
        port
    } else {
        try_connect_to_com()
    }
}

fn try_connect_to_com() -> Result<Box<dyn SerialPort>, Error> {
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
        println!("Connected to esp32: {}", port.port_name);
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
                    eprintln!(
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
                eprintln!("Error reading from serial port: {}", e);
                return Err(e);
            }
        }
    }
}

fn write_if_possible(con: &mut Box<dyn SerialPort>, command: LedbarCommand) -> Result<(), Error> {
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
