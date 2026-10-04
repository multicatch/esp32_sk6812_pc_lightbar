#![windows_subsystem = "windows"]

use std::error::Error;
use std::fs::File;
use esp32_sk6812_lightbar_agent::LedbarCommand::{ShutDown, Sleep, Wake};
use esp32_sk6812_lightbar_agent::{LedbarCommandMessage, init_usb_comm_and_queues};
use log::{error, info};
use std::sync::mpsc::Sender;
use clap::Parser;
use env_logger::Target;
use windows::Win32::Foundation::{HANDLE, HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::Power::{
    HPOWERNOTIFY, RegisterSuspendResumeNotification, UnregisterSuspendResumeNotification,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CreateWindowExW, DEVICE_NOTIFY_WINDOW_HANDLE, DefWindowProcW, DispatchMessageW,
    GWLP_USERDATA, GetMessageW, GetWindowLongPtrW, MSG, PBT_APMRESUMEAUTOMATIC, PBT_APMSUSPEND,
    RegisterClassW, SetWindowLongPtrW, WINDOW_EX_STYLE, WM_ENDSESSION, WM_NCCREATE,
    WM_POWERBROADCAST, WNDCLASSW,
};
use windows::core::PCWSTR;

const CLASS_NAME: PCWSTR = windows::core::w!("SK6812LightbarAgent");

pub struct WindowHandle {
    power_event_sub: HPOWERNOTIFY,
}

impl WindowHandle {
    fn unregister_subs(&self) {
        unsafe {
            if let Err(e) = UnregisterSuspendResumeNotification(self.power_event_sub) {
                error!(
                    "Error when unregistering subscription to power events: {}",
                    e
                );
                // Oh well, it probably means we were never registered in the first place
                // or Windows just rejected our sub... :c Nothing to worry about, anyways.
            }
        }
    }
}

impl Drop for WindowHandle {
    fn drop(&mut self) {
        if self.power_event_sub.is_invalid() {
            return;
        }
        self.unregister_subs();
    }
}

unsafe extern "system" fn wnd_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    info!("Received system message: {} (event {})", msg, wparam.0);
    unsafe {
        if msg == WM_NCCREATE {
            let create_struct = &*(lparam.0 as *const CREATESTRUCTW);
            let tx_ptr = create_struct.lpCreateParams as *mut Sender<LedbarCommandMessage>;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, tx_ptr as isize);
            LRESULT(1)
        } else if msg == WM_POWERBROADCAST {
            let tx_ptr =
                GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const Sender<LedbarCommandMessage>;
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
            let tx_ptr =
                GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const Sender<LedbarCommandMessage>;
            if !tx_ptr.is_null() && wparam.0 != 0 {
                (&*tx_ptr)
                    .send(LedbarCommandMessage::new(ShutDown))
                    .unwrap();
            }
            LRESULT(0)
        } else {
            DefWindowProcW(hwnd, msg, wparam, lparam)
        }
    }
}

#[derive(Parser, Debug)]
#[command(version)]
struct Cli {
    #[arg(long, help = "Set logging level (trace, debug, info, warn, error). Default is 'warn'")]
    log: Option<log::Level>,
    #[arg(long, help = "Set output log file path")]
    log_file: Option<String>,
}

fn main() -> windows::core::Result<()> {
    if let Err(e) = init_logger() {
        panic!("Failed to initialize logger: {}", e);
    };

    let tx = init_usb_comm_and_queues();
    let handle = create_hidden_window(&tx)?;

    unsafe {
        let mut msg = MSG::default();
        while GetMessageW(&mut msg, None, 0, 0).as_bool() {
            DispatchMessageW(&msg);
        }
    }

    handle.unregister_subs();
    Ok(())
}

fn init_logger() -> Result<(), Box<dyn Error>> {
    let args = Cli::parse();

    let mut builder = env_logger::Builder::new();
    let log_level = if let Some(level) = args.log {
        level.to_string()
    } else {
        std::env::var("RUST_LOG").unwrap_or(String::from("warn"))
    };
    builder.parse_filters(&log_level);

    if let Some(log_file_path) = args.log_file {
        let file = File::create(log_file_path)?;
        builder.target(Target::Pipe(Box::new(file)));
    }
    builder.init();
    Ok(())
}

fn create_hidden_window(tx: &Sender<LedbarCommandMessage>) -> windows::core::Result<WindowHandle> {
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
        )?;

        if hwnd.is_invalid() {
            panic!("CreateWindowExW failed.");
        }

        let power_event_sub =
            RegisterSuspendResumeNotification(HANDLE(hwnd.0), DEVICE_NOTIFY_WINDOW_HANDLE)?;

        Ok(WindowHandle { power_event_sub })
    }
}
