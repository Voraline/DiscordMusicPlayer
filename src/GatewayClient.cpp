#include "Voice/GatewayClient.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "Voice/VoiceWebSocket.h"

namespace Voice {

namespace {

std::string StringField(const Json& Data, const char* Key) {
	const auto Found = Data.find(Key);
	if (Found == Data.end() || !Found->is_string()) {
		return {};
	}
	return Found->get<std::string>();
}

}

GatewayClient::GatewayClient(std::string Token, bool SelfBot, bool Debug)
	: Token_(std::move(Token)), SelfBot_(SelfBot), Debug_(Debug) {
	Ws_ = std::make_unique<VoiceWebSocket>(
		"wss://gateway.discord.gg/?v=10&encoding=json",
		11,
		1,
		Debug_
	);

	Ws_->HeartbeatPayload = [this] {
		const std::int64_t Current = Sequence_.load();
		return Current < 0 ? Json(nullptr) : Json(Current);
	};
	Ws_->OnPacket = [this](const Json& Packet) { HandlePacket(Packet); };
	Ws_->OnError = [this](const std::string& Message) { Fail(Message); };
	Ws_->OnClose = [this](int Code) {
		Ready_ = false;
		if (OnClose) {
			OnClose(Code);
		}
	};
	Ws_->OnDebug = [this](const std::string& Message) {
		if (!Debug_ || !OnDebug) {
			return;
		}
		const bool Useful = Message.find("op 10") != std::string::npos
			|| Message.find("op 0 READY") != std::string::npos
			|| Message.find("op 0 VOICE_SERVER_UPDATE") != std::string::npos
			|| Message.find("op 0 VOICE_STATE_UPDATE") != std::string::npos
			|| Message.find("op 7") != std::string::npos
			|| Message.find("op 9") != std::string::npos
			|| Message.find(">> op 2") != std::string::npos
			|| Message.find(">> op 4") != std::string::npos;
		if (Useful) {
			OnDebug("[GW] " + Message);
		}
	};
}

GatewayClient::~GatewayClient() {
	Close();
}

void GatewayClient::Connect() {
	Ws_->Connect();
}

void GatewayClient::SendIdentify() {
	Json Properties;
	Properties["os"] = "Windows";
	Properties["browser"] = SelfBot_ ? "Chrome" : "voice";
	Properties["device"] = "";

	Json Data;
	Data["token"] = Token_;
	if (!SelfBot_) {
		Data["intents"] = 129;
	}
	Data["properties"] = std::move(Properties);

	Json Packet;
	Packet["op"] = 2;
	Packet["d"] = std::move(Data);

	Ws_->SendPacket(Packet);
}

void GatewayClient::HandlePacket(const Json& Packet) {
	if (!Packet.contains("op") || !Packet["op"].is_number_integer()) {
		return;
	}

	if (Packet.contains("s") && Packet["s"].is_number_integer()) {
		Sequence_ = Packet["s"].get<std::int64_t>();
	}

	const int Opcode = Packet["op"].get<int>();

	if (Opcode == 10 && Packet.contains("d")) {
		const Json& Data = Packet["d"];
		if (Data.contains("heartbeat_interval") && Data["heartbeat_interval"].is_number_integer()) {
			Ws_->SetHeartbeatInterval(Data["heartbeat_interval"].get<int>());
		}
		SendIdentify();
		return;
	}

	if (Opcode == 9) {
		Fail("Gateway session invalidated");
		return;
	}

	if (Opcode != 0 || !Packet.contains("t") || !Packet["t"].is_string()
		|| !Packet.contains("d") || !Packet["d"].is_object()) {
		return;
	}

	const std::string Event = Packet["t"].get<std::string>();
	const Json& Data = Packet["d"];

	if (Event == "READY") {
		{
			std::lock_guard Lock(Mutex_);
			if (Data.contains("user") && Data["user"].is_object()) {
				UserId_ = StringField(Data["user"], "id");
			}
			SessionId_ = StringField(Data, "session_id");
			Ready_ = true;
		}
		if (OnReady) {
			OnReady();
		}
		return;
	}

	if (Event == "VOICE_SERVER_UPDATE") {
		const std::string GuildId = StringField(Data, "guild_id");
		const std::string Endpoint = StringField(Data, "endpoint");
		const std::string Token = StringField(Data, "token");

		{
			std::lock_guard Lock(Mutex_);
			if (GuildId != Config_.GuildId || Endpoint.empty()) {
				return;
			}
			Server_.GuildId = GuildId;
			Server_.Endpoint = Endpoint;
			Server_.Token = Token;
			HasServer_ = true;
			VoiceNotified_ = false;
		}
		MaybeNotify();
		return;
	}

	if (Event == "VOICE_STATE_UPDATE") {
		const std::string GuildId = StringField(Data, "guild_id");
		const std::string UserId = StringField(Data, "user_id");

		{
			std::lock_guard Lock(Mutex_);
			if (GuildId != Config_.GuildId || UserId != UserId_) {
				return;
			}
			State_.GuildId = GuildId;
			State_.UserId = UserId;
			State_.SessionId = StringField(Data, "session_id");
			State_.ChannelId = StringField(Data, "channel_id");
			HasState_ = true;
			VoiceNotified_ = false;
			if (Debug_ && OnDebug) {
				OnDebug(
					"[GW] self voice state channel " + State_.ChannelId + " session "
					+ State_.SessionId
				);
			}
		}
		MaybeNotify();
	}
}

void GatewayClient::MaybeNotify() {
	VoiceServerInfo Server;
	VoiceStateInfo State;

	{
		std::lock_guard Lock(Mutex_);
		if (!HasServer_ || !HasState_ || VoiceNotified_) {
			return;
		}
		Server = Server_;
		State = State_;
		VoiceNotified_ = true;
		HasServer_ = false;
		HasState_ = false;
	}

	if (OnVoiceReady) {
		OnVoiceReady(Server, State);
	}
}

void GatewayClient::Join(const JoinConfig& Config) {
	{
		std::lock_guard Lock(Mutex_);
		Config_ = Config;
		if (!Ready_) {
			return;
		}
	}
	SendVoiceState();
}

void GatewayClient::SendVoiceState() {
	JoinConfig Config;
	{
		std::lock_guard Lock(Mutex_);
		if (!Ready_) {
			return;
		}
		Config = Config_;
	}

	Json Data;
	Data["guild_id"] = Config.GuildId;
	Data["channel_id"] = Config.ChannelId.empty() ? Json(nullptr) : Json(Config.ChannelId);
	Data["self_deaf"] = Config.SelfDeaf;
	Data["self_mute"] = Config.SelfMute;
	if (SelfBot_) {
		Data["self_video"] = false;
		Data["flags"] = 2;
	}

	Json Packet;
	Packet["op"] = 4;
	Packet["d"] = std::move(Data);

	if (Debug_ && OnDebug) {
		OnDebug("[GW] sending voice state update for channel " + Config.ChannelId);
	}
	Ws_->SendPacket(Packet);
}

void GatewayClient::Leave() {
	{
		std::lock_guard Lock(Mutex_);
		Config_.ChannelId.clear();
	}
	SendVoiceState();
}

void GatewayClient::Close() {
	if (Ws_) {
		Ws_->Destroy();
	}
	Ready_ = false;
}

bool GatewayClient::IsReady() const {
	return Ready_.load();
}

bool GatewayClient::IsVoiceReady() const {
	std::lock_guard Lock(Mutex_);
	return VoiceNotified_ || (HasServer_ && HasState_);
}

std::string GatewayClient::UserId() const {
	std::lock_guard Lock(Mutex_);
	return UserId_;
}

void GatewayClient::Fail(const std::string& Message) {
	if (OnError) {
		OnError(Message);
	}
}

}
