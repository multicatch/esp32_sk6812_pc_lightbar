use std::sync::mpsc;
use std::sync::mpsc::{Receiver, Sender};

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
