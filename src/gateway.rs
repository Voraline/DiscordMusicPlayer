use std::sync::Arc;

use parking_lot::Mutex;
use serde_json::{json, Value};

use crate::types::{JoinConfig, VoiceServerInfo, VoiceStateInfo};
use crate::ws::{HeartbeatKind, WsClient, WsConn, WsEvent};

type ReadyCallback = Arc<dyn Fn() + Send + Sync>;
type VoiceReadyCallback = Arc<dyn Fn(VoiceServerInfo, VoiceStateInfo) + Send + Sync>;
type MessageCallback = Arc<dyn Fn(String) + Send + Sync>;
type CloseCallback = Arc<dyn Fn(i32) + Send + Sync>;

#[derive(Default)]
struct Callbacks {
    on_ready: Option<ReadyCallback>,
    on_voice_ready: Option<VoiceReadyCallback>,
    on_error: Option<MessageCallback>,
    on_debug: Option<MessageCallback>,
    on_close: Option<CloseCallback>,
}

struct GatewayState {
    ready: bool,
    user_id: String,
    session_id: String,
    config: JoinConfig,
    server: Option<VoiceServerInfo>,
    voice_state: Option<VoiceStateInfo>,
    notified: bool,
}

pub struct GatewayClient {
    inner: Arc<GatewayInner>,
    ws: Mutex<Option<WsClient>>,
}

struct GatewayInner {
    token: String,
    self_bot: bool,
    debug: bool,
    state: Mutex<GatewayState>,
    callbacks: Mutex<Callbacks>,
    conn: Mutex<Option<WsConn>>,
}

impl GatewayClient {
    pub fn new(token: String, self_bot: bool, debug: bool) -> Arc<Self> {
        let inner = Arc::new(GatewayInner {
            token,
            self_bot,
            debug,
            state: Mutex::new(GatewayState {
                ready: false,
                user_id: String::new(),
                session_id: String::new(),
                config: JoinConfig::default(),
                server: None,
                voice_state: None,
                notified: false,
            }),
            callbacks: Mutex::new(Callbacks::default()),
            conn: Mutex::new(None),
        });

        Arc::new(Self {
            inner,
            ws: Mutex::new(None),
        })
    }

    pub fn set_on_ready<F>(&self, callback: F)
    where
        F: Fn() + Send + Sync + 'static,
    {
        self.inner.callbacks.lock().on_ready = Some(Arc::new(callback));
    }

    pub fn set_on_voice_ready<F>(&self, callback: F)
    where
        F: Fn(VoiceServerInfo, VoiceStateInfo) + Send + Sync + 'static,
    {
        self.inner.callbacks.lock().on_voice_ready = Some(Arc::new(callback));
    }

    pub fn set_on_error<F>(&self, callback: F)
    where
        F: Fn(String) + Send + Sync + 'static,
    {
        self.inner.callbacks.lock().on_error = Some(Arc::new(callback));
    }

    pub fn set_on_debug<F>(&self, callback: F)
    where
        F: Fn(String) + Send + Sync + 'static,
    {
        self.inner.callbacks.lock().on_debug = Some(Arc::new(callback));
    }

    pub fn set_on_close<F>(&self, callback: F)
    where
        F: Fn(i32) + Send + Sync + 'static,
    {
        self.inner.callbacks.lock().on_close = Some(Arc::new(callback));
    }

    pub fn connect(&self) -> bool {
        let inner = self.inner.clone();
        let handler: crate::ws::WsHandler =
            Arc::new(move |event, conn| handle_event(&inner, event, conn));

        match WsClient::connect(
            "wss://gateway.discord.gg/?v=10&encoding=json",
            HeartbeatKind::Gateway,
            handler,
        ) {
            Ok(client) => {
                *self.inner.conn.lock() = Some(client.conn());
                *self.ws.lock() = Some(client);
                true
            }
            Err(error) => {
                emit_error(&self.inner, format!("Gateway connect failed: {error}"));
                let callback = self.inner.callbacks.lock().on_close.clone();
                if let Some(callback) = callback {
                    callback(0);
                }
                false
            }
        }
    }

    pub fn join(&self, config: &JoinConfig) {
        let send = {
            let mut state = self.inner.state.lock();
            state.config = config.clone();
            state.ready
        };
        if send {
            send_voice_state(&self.inner);
        }
    }

    pub fn leave(&self) {
        let send = {
            let mut state = self.inner.state.lock();
            state.config.channel_id.clear();
            state.ready
        };
        if send {
            send_voice_state(&self.inner);
        }
    }

    pub fn close(&self) {
        if let Some(mut client) = self.ws.lock().take() {
            client.stop();
        }
        self.inner.state.lock().ready = false;
    }

    pub fn is_ready(&self) -> bool {
        self.inner.state.lock().ready
    }

    pub fn is_voice_ready(&self) -> bool {
        let state = self.inner.state.lock();
        state.notified || (state.server.is_some() && state.voice_state.is_some())
    }

    pub fn user_id(&self) -> String {
        self.inner.state.lock().user_id.clone()
    }
}

fn handle_event(inner: &Arc<GatewayInner>, event: WsEvent, conn: &WsConn) {
    match event {
        WsEvent::Open => {
            debug(inner, "[GW] connected".to_string());
        }
        WsEvent::Json(packet) => handle_packet(inner, &packet, conn),
        WsEvent::Binary { .. } => {}
        WsEvent::Close(code) => {
            inner.state.lock().ready = false;
            let callback = inner.callbacks.lock().on_close.clone();
            if let Some(callback) = callback {
                callback(code);
            }
        }
        WsEvent::Error(message) => emit_error(inner, message),
    }
}

fn handle_packet(inner: &Arc<GatewayInner>, packet: &Value, conn: &WsConn) {
    let Some(opcode) = packet.get("op").and_then(Value::as_i64) else {
        return;
    };

    if opcode == 10 {
        if let Some(interval) = packet
            .get("d")
            .and_then(|data| data.get("heartbeat_interval"))
            .and_then(Value::as_u64)
        {
            conn.set_heartbeat_interval(interval);
        }
        debug(inner, "[GW] >> op 2".to_string());
        conn.send_json(&identify(inner));
        return;
    }

    if opcode == 9 {
        emit_error(inner, "Gateway session invalidated".to_string());
        return;
    }

    if opcode != 0 {
        return;
    }

    let Some(event) = packet.get("t").and_then(Value::as_str) else {
        return;
    };
    let Some(data) = packet.get("d") else {
        return;
    };

    match event {
        "READY" => {
            {
                let mut state = inner.state.lock();
                if let Some(user_id) = data
                    .get("user")
                    .and_then(|user| user.get("id"))
                    .and_then(Value::as_str)
                {
                    state.user_id = user_id.to_string();
                }
                state.session_id = data
                    .get("session_id")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string();
                state.ready = true;
            }
            debug(inner, "[GW] op 0 READY".to_string());
            let callback = inner.callbacks.lock().on_ready.clone();
            if let Some(callback) = callback {
                callback();
            }
        }
        "VOICE_SERVER_UPDATE" => {
            let guild_id = string_field(data, "guild_id");
            let endpoint = string_field(data, "endpoint");
            let token = string_field(data, "token");

            {
                let mut state = inner.state.lock();
                if guild_id != state.config.guild_id || endpoint.is_empty() {
                    return;
                }
                state.server = Some(VoiceServerInfo {
                    token,
                    guild_id,
                    endpoint,
                });
                state.notified = false;
            }
            debug(inner, "[GW] op 0 VOICE_SERVER_UPDATE".to_string());
            maybe_notify(inner);
        }
        "VOICE_STATE_UPDATE" => {
            let guild_id = string_field(data, "guild_id");
            let user_id = string_field(data, "user_id");

            {
                let mut state = inner.state.lock();
                if guild_id != state.config.guild_id || user_id != state.user_id {
                    return;
                }
                let session_id = string_field(data, "session_id");
                let channel_id = string_field(data, "channel_id");
                if inner.debug {
                    debug(
                        inner,
                        format!(
                            "[GW] self voice state channel {} session {}",
                            channel_id, session_id
                        ),
                    );
                }
                state.voice_state = Some(VoiceStateInfo {
                    session_id,
                    channel_id,
                    user_id,
                    guild_id,
                });
                state.notified = false;
            }
            maybe_notify(inner);
        }
        _ => {}
    }
}

fn maybe_notify(inner: &Arc<GatewayInner>) {
    let pair = {
        let mut state = inner.state.lock();
        if state.notified {
            return;
        }
        match (state.server.take(), state.voice_state.take()) {
            (Some(server), Some(voice_state)) => {
                state.notified = true;
                Some((server, voice_state))
            }
            (server, voice_state) => {
                state.server = server;
                state.voice_state = voice_state;
                None
            }
        }
    };

    if let Some((server, voice_state)) = pair {
        let callback = inner.callbacks.lock().on_voice_ready.clone();
        if let Some(callback) = callback {
            callback(server, voice_state);
        }
    }
}

fn identify(inner: &GatewayInner) -> Value {
    let properties = json!({
        "os": "Windows",
        "browser": if inner.self_bot { "Chrome" } else { "voice" },
        "device": "",
    });

    let mut data = json!({
        "token": inner.token,
        "properties": properties,
    });
    if !inner.self_bot {
        data["intents"] = json!(129);
    }

    json!({ "op": 2, "d": data })
}

fn send_voice_state(inner: &GatewayInner) {
    let (config, self_bot) = {
        let state = inner.state.lock();
        (state.config.clone(), inner.self_bot)
    };

    let channel_id = if config.channel_id.is_empty() {
        Value::Null
    } else {
        json!(config.channel_id)
    };

    let mut data = json!({
        "guild_id": config.guild_id,
        "channel_id": channel_id,
        "self_deaf": config.self_deaf,
        "self_mute": config.self_mute,
    });
    if self_bot {
        data["self_video"] = json!(false);
        data["flags"] = json!(2);
    }

    if inner.debug {
        debug(
            inner,
            format!("[GW] sending voice state update for channel {}", config.channel_id),
        );
    }

    if let Some(conn) = inner.conn.lock().as_ref() {
        conn.send_json(&json!({ "op": 4, "d": data }));
    }
}

fn string_field(data: &Value, key: &str) -> String {
    data.get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string()
}

fn emit_error(inner: &GatewayInner, message: String) {
    let callback = inner.callbacks.lock().on_error.clone();
    if let Some(callback) = callback {
        callback(message);
    }
}

fn debug(inner: &GatewayInner, message: String) {
    if !inner.debug {
        return;
    }
    let callback = inner.callbacks.lock().on_debug.clone();
    if let Some(callback) = callback {
        callback(message);
    }
}
