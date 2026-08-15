#![windows_subsystem = "windows"]

use serialport::{Error, SerialPort};
use std::io::Write;
use std::sync::mpsc;
use std::sync::mpsc::Sender;
use std::thread;
use std::thread::sleep;
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DispatchMessageW, GWLP_USERDATA, GetMessageW,
    GetWindowLongPtrW, MSG, PBT_APMSUSPEND, RegisterClassW, SetWindowLongPtrW, WINDOW_EX_STYLE,
    WM_ENDSESSION, WM_NCCREATE, WM_POWERBROADCAST, WNDCLASSW,
};
use windows::core::PCWSTR;

const CLASS_NAME: PCWSTR = windows::core::w!("SK6812LightbarAgent");

#[derive(Debug, PartialEq)]
enum LedbarCommand {
    Wake,
    Sleep,
    ShutDown,
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
            let tx_ptr = create_struct.lpCreateParams as *mut Sender<LedbarCommand>;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, tx_ptr as isize);
            LRESULT(1)
        } else if msg == WM_POWERBROADCAST {
            let tx_ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const Sender<LedbarCommand>;
            if !tx_ptr.is_null() {
                match wparam.0 as u32 {
                    PBT_APMSUSPEND => {
                        (&*tx_ptr).send(LedbarCommand::Sleep).unwrap();
                    }
                    PBT_APMRESUMEAUTOMATIC => {
                        (&*tx_ptr).send(LedbarCommand::Wake).unwrap();
                    }
                    _ => {}
                }
            }
            LRESULT(1)
        } else if msg == WM_ENDSESSION {
            let tx_ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const Sender<LedbarCommand>;
            if !tx_ptr.is_null() {
                if wparam.0 != 0 {
                    (&*tx_ptr).send(LedbarCommand::ShutDown).unwrap();
                }
            }
            LRESULT(0)
        } else {
            DefWindowProcW(hwnd, msg, wparam, lparam)
        }
    }
}

fn main() -> windows::core::Result<()> {
    let (tx, rx) = mpsc::channel::<LedbarCommand>();

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

        if (hwnd?.is_invalid()) {
            panic!("CreateWindowExW failed.");
        }
    }

    thread::spawn(move || {
        let mut port = serialport::new("COM3", 115_200)
            .timeout(Duration::from_millis(500))
            .open();

        let mut last_sleep = Instant::now();

        while let Ok(command) = rx.recv() {
            if command == LedbarCommand::Wake && last_sleep.elapsed() < Duration::from_secs(10) {
                continue; // some background tasks may wake us up too early
            }

            println!("Received command {:?}", command);
            port = retry_if_closed(port);
            match port {
                Ok(ref mut con) => {
                    if command == LedbarCommand::Sleep {
                        last_sleep = Instant::now();
                    }
                    if let Err(e) = write_if_possible(con, command) {
                        println!("Cannot write command! {}", e);
                    }
                }
                Err(ref err) => {
                    println!("Cannot open port! {}", err.clone());
                }
            }
        }
    });

    thread::spawn(move || {
        loop {
            sleep(Duration::from_millis(500));
            tx.send(LedbarCommand::Wake).unwrap();
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

fn retry_if_closed(port: Result<Box<dyn SerialPort>, Error>) -> Result<Box<dyn SerialPort>, Error> {
    if port.is_ok() {
        port
    } else {
        serialport::new("COM3", 115_200)
            .timeout(Duration::from_millis(500))
            .open()
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
