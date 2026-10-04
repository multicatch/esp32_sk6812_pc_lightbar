use crate::LedbarCommand::{Sleep, Wake};
use crate::broadcast::Broadcast;
use crate::usb::{retry_if_closed, try_connect_to_com, write_if_possible};
use log::{debug, error, trace};
use std::sync::mpsc::{Receiver, Sender};
use std::thread;
use std::thread::sleep;
use std::time::{Duration, Instant};

pub mod usb;
pub mod broadcast;

/// USB VID of ESP32
pub const ESPRESSIF_VID: u16 = 0x303A;

/// Greeting used by the ESP32 controller
pub const CONTROLLER_GREETING: &str = "ESP32-SK6812-LIGHTBAR-V1";

/// Time offset of a message in queue that will be considered "stale" (too old) and rejected
pub const STALE_MESSAGE_TIME: Duration = Duration::from_secs(5);

/// After receiving "sleep" event from Windows, we will ignore messages for some time to be sure nothing wakes up the LEDs.
pub const SLEEP_CMD_QUIET_TIME: Duration = Duration::from_secs(10);

/// How long to pause the "ping"/"heartbeat" after "sleep" event is received
pub const PING_PAUSE_TIME: Duration = Duration::from_mins(5);


#[derive(Debug, Clone)]
pub struct LedbarCommandMessage(pub Instant, pub LedbarCommand);

impl LedbarCommandMessage {
    pub fn new(cmd: LedbarCommand) -> Self {
        LedbarCommandMessage(Instant::now(), cmd)
    }
}

#[derive(Debug, PartialEq, Clone)]
pub enum LedbarCommand {
    Wake,
    Sleep,
    ShutDown,
}

/// Initializes a broadcast queue for system events and USB communication (and spawns threads to handle all of that).
///
/// What it actually does:
/// * creates a broadcast queue for events
/// * runs a thread that allows the broadcast to send the events to every subscriber
/// * runs a thread that communicates with the ESP32 via USB
/// * runs a thread that sends a heartbeat to the LEDs (so they will stay on)
pub fn init_usb_comm_and_queues() -> Sender<LedbarCommandMessage> {
    let (tx, mut broadcast) = Broadcast::<LedbarCommandMessage>::new();
    let esp32_rx = broadcast.create_subscriber();
    let ping_rx = broadcast.create_subscriber();

    thread::spawn(move || {
        broadcast.run_broadcast();
    });

    thread::spawn(move || {
        esp32_usb_com_blocking_task(esp32_rx);
    });

    tx.send(LedbarCommandMessage::new(Wake)).unwrap(); // first heartbeat

    let heartbeat_tx = tx.clone();
    thread::spawn(move || {
        heartbeat_blocking_task(heartbeat_tx, ping_rx);
    });
    tx
}

/// Runs a blocking task that will (in a loop) send any task received from the queue (to the ESP32 connected via USB)
pub fn esp32_usb_com_blocking_task(esp32_rx: Receiver<LedbarCommandMessage>) {
    let mut port = try_connect_to_com();
    let mut last_sleep = Instant::now();

    while let Ok(LedbarCommandMessage(time, command)) = esp32_rx.recv() {
        trace!("Received command {:?} (emitted at {:?}), deciding what to do...", command, time);
        if time.elapsed() > STALE_MESSAGE_TIME {
            // stale message
            continue;
        }
        if command == Wake && last_sleep.elapsed() < SLEEP_CMD_QUIET_TIME {
            continue; // some background tasks may wake us up too early
        }

        debug!("Received command {:?} (emitted at {:?})", command, time);
        port = retry_if_closed(port);
        match port {
            Ok(ref mut con) => {
                if command == Sleep {
                    last_sleep = Instant::now();
                }
                if let Err(e) = write_if_possible(con, command.clone()) {
                    error!("Cannot write command {:?}! {}", command, e);
                }
            }
            Err(ref err) => {
                error!("Cannot open USB port to ESP32! {}", err.clone());
            }
        }
    }
}

/// Runs a blocking task that will (in a loop) send a heartbeat message to the ESP32 (and the LEDs will stay on when the PC is on)
pub fn heartbeat_blocking_task(tx: Sender<LedbarCommandMessage>, ping_rx: Receiver<LedbarCommandMessage>) {
    let mut paused = false;
    let mut paused_time = Instant::now();
    loop {
        sleep(Duration::from_secs(2));
        if let Ok(LedbarCommandMessage(time, cmd)) = ping_rx.try_recv() {
            if cmd != Wake && time.elapsed() <= STALE_MESSAGE_TIME {
                // slow down, we're either going to sleep or user is trying to shut down the PC
                paused = true;
                paused_time = Instant::now();
            }
            if cmd == Wake {
                // we received "resume" event
                paused = false;
            }
        }
        if paused && paused_time.elapsed() > PING_PAUSE_TIME {
            paused = false;
        }
        if !paused {
            tx.send(LedbarCommandMessage::new(Wake)).unwrap();
        }
    }
}