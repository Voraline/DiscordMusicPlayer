#include "Voice/VoiceWebSocket.h"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace Voice {

namespace {

std::int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::steady_clock::now().time_since_epoch()
	)
		.count();
}

std::int64_t SystemNowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::system_clock::now().time_since_epoch()
	)
		.count();
}

}

struct VoiceWebSocket::Impl {
	std::unique_ptr<ix::WebSocket> Socket;
};

VoiceWebSocket::VoiceWebSocket(std::string Url, int AckOpcode, int HeartbeatOpcode, bool Debug)
	: Impl_(std::make_unique<Impl>()),
	  Url_(std::move(Url)),
	  AckOpcode_(AckOpcode),
	  HeartbeatOpcode_(HeartbeatOpcode),
	  Debug_(Debug) {}

VoiceWebSocket::~VoiceWebSocket() {
	Destroy();
}

void VoiceWebSocket::Connect() {
	bool Expected = false;
	if (!Started_.compare_exchange_strong(Expected, true)) {
		return;
	}

	Impl_->Socket = std::make_unique<ix::WebSocket>();
	Impl_->Socket->setUrl(Url_);
	Impl_->Socket->disableAutomaticReconnection();
	Impl_->Socket->setPingInterval(0);

	ix::SocketTLSOptions TlsOptions;
	TlsOptions.caFile = "SYSTEM";
	Impl_->Socket->setTLSOptions(TlsOptions);

	Impl_->Socket->setOnMessageCallback([this](const ix::WebSocketMessagePtr& Message) {
		if (!Message) {
			return;
		}

		switch (Message->type) {
			case ix::WebSocketMessageType::Open:
				Open_ = true;
				CloseReported_ = false;
				if (OnOpen) {
					OnOpen();
				}
				break;
			case ix::WebSocketMessageType::Close:
				Open_ = false;
				CloseCode_ = Message->closeInfo.code;
				if (!Stop_ && !CloseReported_.exchange(true) && OnClose) {
					OnClose(Message->closeInfo.code);
				}
				break;
			case ix::WebSocketMessageType::Error:
				Open_ = false;
				Fail(Message->errorInfo.reason);
				if (!Stop_ && !CloseReported_.exchange(true) && OnClose) {
					OnClose(0);
				}
				break;
			case ix::WebSocketMessageType::Message:
				if (Message->binary) {
					HandleBinaryMessage(Message->str);
				} else {
					HandleMessage(Message->str);
				}
				break;
			default:
				break;
		}
	});

	HeartbeatThread_ = std::thread([this] { HeartbeatLoop(); });
	Impl_->Socket->start();
}

void VoiceWebSocket::HeartbeatLoop() {
	while (!Stop_) {
		const int Interval = HeartbeatInterval_.load();
		if (Interval <= 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			continue;
		}

		int Remaining = Interval;
		while (Remaining > 0 && !Stop_) {
			const int Step = std::min(Remaining, 25);
			std::this_thread::sleep_for(std::chrono::milliseconds(Step));
			Remaining -= Step;
		}
		if (Stop_) {
			break;
		}

		if (LastHeartbeatSend_.load() != 0 && MissedHeartbeats_.load() >= 3) {
			Close(4000);
			break;
		}

		SendHeartbeat();
		if (!IsOpen()) {
			break;
		}
	}
}

void VoiceWebSocket::SendHeartbeat() {
	Json Data;
	if (ObjectHeartbeat) {
		Data = Json::object();
		Data["t"] = SystemNowMs();
		Data["seq_ack"] = LastSequence_.load();
	} else {
		Data = HeartbeatPayload ? HeartbeatPayload() : Json(nullptr);
	}

	LastHeartbeatSend_ = NowMs();
	MissedHeartbeats_++;

	Json Packet;
	Packet["op"] = HeartbeatOpcode_;
	Packet["d"] = std::move(Data);

	SendPacket(Packet);
}

bool VoiceWebSocket::SendPacket(const Json& Packet) {
	if (Debug_ && OnDebug) {
		const int Opcode = Packet.contains("op") && Packet["op"].is_number_integer()
			? Packet["op"].get<int>()
			: -1;
		OnDebug(">> op " + std::to_string(Opcode));
	}
	return SendText(Packet.dump());
}

bool VoiceWebSocket::SendText(const std::string& Text) {
	if (!Open_.load() || Stop_.load()) {
		return false;
	}

	std::lock_guard Lock(SendMutex_);
	if (!Impl_ || !Impl_->Socket || !Open_.load()) {
		return false;
	}

	const ix::WebSocketSendInfo Info = Impl_->Socket->sendText(Text);
	if (!Info.success) {
		Fail("Failed to send WebSocket text message");
		return false;
	}
	return true;
}

bool VoiceWebSocket::SendBinary(int Opcode, std::span<const std::uint8_t> Payload) {
	if (!Open_.load() || Stop_.load()) {
		return false;
	}

	std::lock_guard Lock(SendMutex_);
	if (!Impl_ || !Impl_->Socket || !Open_.load()) {
		return false;
	}

	std::string Frame;
	Frame.reserve(Payload.size() + 1);
	Frame.push_back(static_cast<char>(static_cast<std::uint8_t>(Opcode & 0xFF)));
	if (!Payload.empty()) {
		Frame.append(
			reinterpret_cast<const char*>(Payload.data()),
			Payload.size()
		);
	}

	if (Debug_ && OnDebug) {
		OnDebug(
			">> binary " + std::to_string(Opcode) + " (" + std::to_string(Payload.size())
			+ " bytes)"
		);
	}

	const ix::WebSocketSendInfo Info = Impl_->Socket->sendBinary(Frame);
	if (!Info.success) {
		Fail("Failed to send WebSocket binary message");
		return false;
	}
	return true;
}

void VoiceWebSocket::HandleMessage(const std::string& Message) {
	Json Packet = Json::parse(Message, nullptr, false);
	if (Packet.is_discarded() || !Packet.is_object()) {
		return;
	}

	if (Packet.contains("seq") && Packet["seq"].is_number_integer()) {
		LastSequence_ = Packet["seq"].get<std::int64_t>();
	}

	if (Debug_ && OnDebug) {
		const int Opcode = Packet.contains("op") && Packet["op"].is_number_integer()
			? Packet["op"].get<int>()
			: -1;
		std::string Line = "<< op " + std::to_string(Opcode);
		if (Packet.contains("t") && Packet["t"].is_string()) {
			Line += " " + Packet["t"].get<std::string>();
		}
		OnDebug(Line);
	}

	if (Packet.contains("op") && Packet["op"].is_number_integer()
		&& Packet["op"].get<int>() == AckOpcode_) {
		const std::int64_t Now = NowMs();
		LastHeartbeatAck_ = Now;
		MissedHeartbeats_ = 0;
		const std::int64_t Sent = LastHeartbeatSend_.load();
		if (Sent != 0) {
			PingMs_ = static_cast<int>(Now - Sent);
		}
	}

	if (OnPacket) {
		OnPacket(Packet);
	}
}

void VoiceWebSocket::HandleBinaryMessage(const std::string& Message) {
	if (Message.size() < 3) {
		return;
	}

	const auto* Bytes = reinterpret_cast<const std::uint8_t*>(Message.data());
	const int Sequence = (static_cast<int>(Bytes[0]) << 8) | static_cast<int>(Bytes[1]);
	LastSequence_ = Sequence;
	const int Opcode = static_cast<int>(Bytes[2]);
	std::vector<std::uint8_t> Payload(Bytes + 3, Bytes + Message.size());

	if (Debug_ && OnDebug) {
		OnDebug(
			"<< binary op " + std::to_string(Opcode) + " seq " + std::to_string(Sequence) + " ("
			+ std::to_string(Payload.size()) + " bytes)"
		);
	}

	if (OnBinary) {
		OnBinary(Opcode, std::move(Payload));
	}
}

void VoiceWebSocket::SetHeartbeatInterval(int Milliseconds) {
	const int Previous = HeartbeatInterval_.exchange(Milliseconds);
	if (ImmediateHeartbeat && Milliseconds > 0 && Previous <= 0) {
		SendHeartbeat();
	}
}

void VoiceWebSocket::Close(int Code) {
	Stop_ = true;
	if (Impl_ && Impl_->Socket) {
		Impl_->Socket->stop(static_cast<std::uint16_t>(Code));
	}
}

void VoiceWebSocket::Destroy() {
	Stop_ = true;
	if (Impl_ && Impl_->Socket) {
		Impl_->Socket->stop();
		Impl_->Socket.reset();
	}
	JoinHeartbeatThread();
}

void VoiceWebSocket::JoinHeartbeatThread() {
	if (!HeartbeatThread_.joinable()) {
		return;
	}
	if (HeartbeatThread_.get_id() == std::this_thread::get_id()) {
		HeartbeatThread_.detach();
		return;
	}
	HeartbeatThread_.join();
}

void VoiceWebSocket::Fail(const std::string& Message) {
	if (OnError) {
		OnError(Message);
	}
}

std::optional<int> VoiceWebSocket::Ping() const {
	const int Value = PingMs_.load();
	if (Value < 0) {
		return std::nullopt;
	}
	return Value;
}

bool VoiceWebSocket::IsOpen() const {
	return Open_.load();
}

}
