use std::io::ErrorKind;
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde_json::{json, Value};
use tungstenite::stream::MaybeTlsStream;
use tungstenite::{Message, WebSocket};

const READ_POLL: Duration = Duration::from_millis(10);
const HEARTBEAT_STEP: Duration = Duration::from_millis(25);

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum HeartbeatKind {
    Gateway,
    Voice,
}

pub enum WsEvent {
    Open,
    Json(Value),
    Binary { opcode: u8, payload: Vec<u8> },
    Close(i32),
    Error(String),
}

enum Outgoing {
    Text(String),
    Binary(Vec<u8>),
    Close(u16),
}

#[derive(Clone)]
pub struct WsConn {
    sender: Sender<Outgoing>,
    heartbeat_interval: Arc<AtomicU64>,
    last_sequence: Arc<AtomicI64>,
    stop: Arc<AtomicBool>,
}

impl WsConn {
    pub fn send_json(&self, packet: &Value) {
        if let Ok(text) = serde_json::to_string(packet) {
            let _ = self.sender.send(Outgoing::Text(text));
        }
    }

    pub fn send_binary(&self, opcode: u8, payload: &[u8]) {
        let mut frame = Vec::with_capacity(payload.len() + 1);
        frame.push(opcode);
        frame.extend_from_slice(payload);
        let _ = self.sender.send(Outgoing::Binary(frame));
    }

    pub fn set_heartbeat_interval(&self, milliseconds: u64) {
        self.heartbeat_interval.store(milliseconds, Ordering::Relaxed);
    }

    pub fn last_sequence(&self) -> i64 {
        self.last_sequence.load(Ordering::Relaxed)
    }

    pub fn stop(&self) {
        self.stop.store(true, Ordering::Relaxed);
        let _ = self.sender.send(Outgoing::Close(1000));
    }
}

pub type WsHandler = Arc<dyn Fn(WsEvent, &WsConn) + Send + Sync>;

pub struct WsClient {
    conn: WsConn,
    reader: Option<JoinHandle<()>>,
    heartbeat: Option<JoinHandle<()>>,
}

impl WsClient {
    pub fn connect(
        url: &str,
        heartbeat: HeartbeatKind,
        handler: WsHandler,
    ) -> Result<Self, tungstenite::Error> {
        let (mut socket, _) = tungstenite::connect(url)?;
        set_read_timeout(socket.get_ref(), Some(READ_POLL))?;

        let (sender, receiver) = mpsc::channel();
        let stop = Arc::new(AtomicBool::new(false));
        let interval = Arc::new(AtomicU64::new(0));
        let last_sequence = Arc::new(AtomicI64::new(if heartbeat == HeartbeatKind::Gateway {
            -1
        } else {
            0
        }));

        let conn = WsConn {
            sender,
            heartbeat_interval: interval.clone(),
            last_sequence: last_sequence.clone(),
            stop: stop.clone(),
        };

        let reader = {
            let conn = conn.clone();
            let handler = handler.clone();
            let reader_stop = stop.clone();
            thread::spawn(move || {
                read_loop(&mut socket, &receiver, &conn, &handler, &reader_stop);
            })
        };

        let heartbeat_thread = {
            let conn = conn.clone();
            let heartbeat_stop = stop.clone();
            thread::spawn(move || heartbeat_loop(&conn, heartbeat, &heartbeat_stop))
        };

        Ok(Self {
            conn,
            reader: Some(reader),
            heartbeat: Some(heartbeat_thread),
        })
    }

    pub fn conn(&self) -> WsConn {
        self.conn.clone()
    }

    pub fn stop(&mut self) {
        self.conn.stop();
        if let Some(handle) = self.reader.take() {
            if handle.thread().id() != thread::current().id() {
                let _ = handle.join();
            }
        }
        if let Some(handle) = self.heartbeat.take() {
            if handle.thread().id() != thread::current().id() {
                let _ = handle.join();
            }
        }
    }
}

impl Drop for WsClient {
    fn drop(&mut self) {
        self.stop();
    }
}

fn set_read_timeout(
    stream: &MaybeTlsStream<TcpStream>,
    timeout: Option<Duration>,
) -> std::io::Result<()> {
    match stream {
        MaybeTlsStream::Plain(tcp) => tcp.set_read_timeout(timeout),
        MaybeTlsStream::Rustls(tls) => tls.sock.set_read_timeout(timeout),
        _ => Ok(()),
    }
}

fn read_loop(
    socket: &mut WebSocket<MaybeTlsStream<TcpStream>>,
    receiver: &Receiver<Outgoing>,
    conn: &WsConn,
    handler: &WsHandler,
    stop: &AtomicBool,
) {
    let mut close_code = 1005i32;
    handler(WsEvent::Open, conn);

    loop {
        while let Ok(outgoing) = receiver.try_recv() {
            let sent = match outgoing {
                Outgoing::Text(text) => socket.send(Message::text(text)),
                Outgoing::Binary(bytes) => socket.send(Message::binary(bytes)),
                Outgoing::Close(code) => {
                    let _ = socket.close(Some(tungstenite::protocol::CloseFrame {
                        code: code.into(),
                        reason: "".into(),
                    }));
                    let _ = socket.flush();
                    return;
                }
            };
            if let Err(error) = sent {
                if !stop.load(Ordering::Relaxed) {
                    handler(WsEvent::Error(error.to_string()), conn);
                }
                let _ = socket.close(None);
                return;
            }
        }

        if stop.load(Ordering::Relaxed) {
            let _ = socket.close(None);
            return;
        }

        match socket.read() {
            Ok(Message::Text(text)) => {
                if let Ok(value) = serde_json::from_str::<Value>(&text) {
                    if let Some(sequence) = value.get("s").and_then(Value::as_i64) {
                        conn.last_sequence.store(sequence, Ordering::Relaxed);
                    }
                    handler(WsEvent::Json(value), conn);
                }
            }
            Ok(Message::Binary(bytes)) => {
                if bytes.len() >= 3 {
                    let sequence =
                        ((bytes[0] as i64) << 8) | bytes[1] as i64;
                    conn.last_sequence.store(sequence, Ordering::Relaxed);
                    handler(
                        WsEvent::Binary {
                            opcode: bytes[2],
                            payload: bytes[3..].to_vec(),
                        },
                        conn,
                    );
                }
            }
            Ok(Message::Ping(payload)) => {
                let _ = socket.send(Message::Pong(payload));
            }
            Ok(Message::Pong(_)) | Ok(Message::Frame(_)) => {}
            Ok(Message::Close(frame)) => {
                if let Some(frame) = frame {
                    close_code = u16::from(frame.code) as i32;
                }
                break;
            }
            Err(tungstenite::Error::Io(error))
                if error.kind() == ErrorKind::WouldBlock || error.kind() == ErrorKind::TimedOut =>
            {
                continue;
            }
            Err(tungstenite::Error::ConnectionClosed) | Err(tungstenite::Error::AlreadyClosed) => {
                break;
            }
            Err(error) => {
                if !stop.load(Ordering::Relaxed) {
                    handler(WsEvent::Error(error.to_string()), conn);
                }
                break;
            }
        }
    }

    let _ = socket.close(None);
    if !stop.load(Ordering::Relaxed) {
        handler(WsEvent::Close(close_code), conn);
    }
}

fn heartbeat_loop(conn: &WsConn, heartbeat: HeartbeatKind, stop: &AtomicBool) {
    loop {
        let interval = conn.heartbeat_interval.load(Ordering::Relaxed);
        if interval == 0 {
            for _ in 0..(HEARTBEAT_STEP.as_millis() as usize / 25) {
                if stop.load(Ordering::Relaxed) {
                    return;
                }
                thread::sleep(HEARTBEAT_STEP);
            }
            continue;
        }

        let mut remaining = interval;
        while remaining > 0 && !stop.load(Ordering::Relaxed) {
            let step = remaining.min(HEARTBEAT_STEP.as_millis() as u64);
            thread::sleep(Duration::from_millis(step));
            remaining -= step;
        }
        if stop.load(Ordering::Relaxed) {
            return;
        }

        let sequence = conn.last_sequence.load(Ordering::Relaxed);
        let packet = match heartbeat {
            HeartbeatKind::Gateway => json!({
                "op": 1,
                "d": if sequence < 0 { Value::Null } else { json!(sequence) },
            }),
            HeartbeatKind::Voice => json!({
                "op": 3,
                "d": {
                    "t": SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .map(|elapsed| elapsed.as_millis() as u64)
                        .unwrap_or(0),
                    "seq_ack": sequence,
                },
            }),
        };
        conn.send_json(&packet);
    }
}
