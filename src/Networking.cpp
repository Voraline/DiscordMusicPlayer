#include "Voice/Networking.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "DaveSession.h"
#include "Platform.h"
#include "Voice/Secretbox.h"
#include "Voice/VoiceUdpSocket.h"
#include "Voice/VoiceWebSocket.h"

namespace Voice {

namespace {

constexpr std::uint16_t TimestampIncrement = 960;
constexpr int DiscoveryTimeoutMs = 5000;

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

std::uint16_t Random16() {
	std::uint8_t Bytes[2]{};
	Secretbox::RandomBytes(Bytes);
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(Bytes[0]) << 8) | Bytes[1]);
}

std::uint32_t Random32() {
	std::uint8_t Bytes[4]{};
	Secretbox::RandomBytes(Bytes);
	return (static_cast<std::uint32_t>(Bytes[0]) << 24)
		| (static_cast<std::uint32_t>(Bytes[1]) << 16)
		| (static_cast<std::uint32_t>(Bytes[2]) << 8)
		| static_cast<std::uint32_t>(Bytes[3]);
}

}

Networking::Networking(ConnectionOptions Options, bool Debug)
	: Options_(std::move(Options)), Debug_(Debug) {
	Secretbox::Init();
	PreparedPacket_.reserve(4096);
	DaveFrame_.reserve(4096);

	Dave_ = std::make_unique<DaveSession>();
	Dave_->OnFailure = [this](const std::string& Message) {
		if (Debug_ && OnDebug) {
			OnDebug("[DAVE] " + Message);
		}
	};
	Dave_->OnDebug = [this](const std::string& Message) {
		if (Debug_ && OnDebug) {
			OnDebug("[DAVE] " + Message);
		}
	};
	if (!Dave_->Available() && Debug_ && OnDebug) {
		OnDebug("[DAVE] libdave.dll not found");
	}

	std::string VoiceEndpoint = Options_.Endpoint;
	if (!VoiceEndpoint.empty() && VoiceEndpoint.back() != '/') {
		VoiceEndpoint.push_back('/');
	}

	Ws_ = std::make_unique<VoiceWebSocket>("wss://" + VoiceEndpoint + "?v=8", 6, 3, Debug_);
	Ws_->OnOpen = [this] {
		SendIdentify();
		SetCode(NetworkingStatusCode::Identifying);
	};
	Ws_->OnPacket = [this](const Json& Packet) { HandlePacket(Packet); };
	Ws_->OnBinary = [this](int Opcode, std::vector<std::uint8_t> Payload) {
		HandleBinary(Opcode, std::move(Payload));
	};
	Ws_->OnError = [this](const std::string& Message) { Fail(Message); };
	Ws_->OnClose = [this](int Code) { HandleClose(Code); };
	Ws_->OnDebug = [this](const std::string& Message) {
		if (Debug_ && OnDebug) {
			OnDebug("[WS] " + Message);
		}
	};
	Ws_->HeartbeatPayload = [] { return Json(SystemNowMs()); };
	Ws_->ObjectHeartbeat = true;
}

void Networking::Start() {
	if (Debug_ && OnDebug) {
		OnDebug(
			"[NW] connecting to " + Options_.Endpoint + " guild " + Options_.ServerId
			+ " user " + Options_.UserId + " session " + Options_.SessionId
		);
	}
	if (Ws_) {
		Ws_->Connect();
	}
}

Networking::~Networking() {
	Code_ = NetworkingStatusCode::Closed;
	if (Udp_) {
		Udp_->Destroy();
	}
	if (DiscoveryThread_.joinable()) {
		DiscoveryThread_.join();
	}
	if (Ws_) {
		Ws_->Destroy();
	}
	Udp_.reset();
	Ws_.reset();
}

void Networking::Destroy() {
	if (Code_.exchange(NetworkingStatusCode::Closed) == NetworkingStatusCode::Closed) {
		return;
	}
	if (Ws_) {
		Ws_->Destroy();
	}
	if (Udp_) {
		Udp_->Destroy();
	}
}

NetworkingStatusCode Networking::Code() const {
	return Code_.load();
}

void Networking::SetCode(NetworkingStatusCode Next) {
	const NetworkingStatusCode Previous = Code_.exchange(Next);
	if (Previous == Next) {
		return;
	}
	if (OnStateChange) {
		OnStateChange(Previous, Next);
	}
	if (Debug_ && OnDebug) {
		OnDebug(
			"state " + std::to_string(static_cast<int>(Previous)) + " -> "
			+ std::to_string(static_cast<int>(Next))
		);
	}
}

void Networking::Fail(const std::string& Message) {
	if (OnError) {
		OnError(Message);
	}
}

void Networking::SendIdentify() {
	Json Data;
	Data["server_id"] = Options_.ServerId;
	Data["user_id"] = Options_.UserId;
	Data["session_id"] = Options_.SessionId;
	Data["token"] = Options_.Token;
	Data["max_dave_protocol_version"] = (Dave_ && Dave_->Available())
		? Dave_->MaxProtocolVersion()
		: 0;

	Json Packet;
	Packet["op"] = 0;
	Packet["d"] = std::move(Data);

	Ws_->SendPacket(Packet);
}

std::optional<std::string> Networking::ChooseEncryptionMode(const std::vector<std::string>& Modes) const {
	const auto Found = std::find(
		Modes.begin(),
		Modes.end(),
		"aead_xchacha20_poly1305_rtpsize"
	);
	if (Found == Modes.end()) {
		return std::nullopt;
	}
	return *Found;
}

void Networking::HandlePacket(const Json& Packet) {
	if (!Packet.contains("op") || !Packet["op"].is_number_integer()) {
		return;
	}

	const int Opcode = Packet["op"].get<int>();

	if (Opcode == 8 && Packet.contains("d")) {
		const Json& Data = Packet["d"];
		if (Data.contains("heartbeat_interval") && Data["heartbeat_interval"].is_number_integer()) {
			Ws_->SetHeartbeatInterval(Data["heartbeat_interval"].get<int>());
		}
		return;
	}

	if (Opcode == 2 && Code_.load() == NetworkingStatusCode::Identifying) {
		if (!Packet.contains("d")) {
			return;
		}
		const Json& Data = Packet["d"];
		const std::string Ip = Data.value("ip", std::string());
		const int Port = Data.value("port", 0);
		Ssrc_ = Data.value("ssrc", 0u);

		std::vector<std::string> Modes;
		if (Data.contains("modes") && Data["modes"].is_array()) {
			for (const Json& Mode : Data["modes"]) {
				if (Mode.is_string()) {
					Modes.push_back(Mode.get<std::string>());
				}
			}
		}

		if (Ip.empty() || Port <= 0) {
			Fail("Invalid voice ready payload");
			HandleClose(0);
			return;
		}

		Udp_ = std::make_unique<VoiceUdpSocket>(
			SocketConfig{Ip, static_cast<std::uint16_t>(Port)},
			Debug_
		);
		Udp_->OnError = [this](const std::string& Message) { Fail(Message); };
		Udp_->OnClose = [this] { HandleUdpClose(); };
		Udp_->OnDebug = [this](const std::string& Message) {
			if (Debug_ && OnDebug) {
				OnDebug("[UDP] " + Message);
			}
		};

		if (!Udp_->Open()) {
			HandleClose(0);
			return;
		}

		SetCode(NetworkingStatusCode::UdpHandshaking);

		DiscoveryThread_ = std::jthread([this, Modes = std::move(Modes)] {
			std::optional<SocketConfig> Local = Udp_
				? Udp_->PerformIPDiscovery(Ssrc_, std::chrono::milliseconds(DiscoveryTimeoutMs))
				: std::nullopt;
			if (!Local) {
				if (Code_.load() != NetworkingStatusCode::Closed) {
					Fail("Voice IP discovery failed");
				}
				return;
			}
			if (Code_.load() != NetworkingStatusCode::UdpHandshaking) {
				return;
			}

			const std::optional<std::string> Mode = ChooseEncryptionMode(Modes);
			if (!Mode) {
				Fail("No supported encryption mode");
				return;
			}

			Json Protocol;
			Protocol["address"] = Local->Ip;
			Protocol["port"] = Local->Port;
			Protocol["mode"] = *Mode;

			Json Data;
			Data["protocol"] = "udp";
			Data["data"] = std::move(Protocol);

			Json Select;
			Select["op"] = 1;
			Select["d"] = std::move(Data);

			Ws_->SendPacket(Select);
			SetCode(NetworkingStatusCode::SelectingProtocol);
		});
		return;
	}

	if (Opcode == 4 && Code_.load() == NetworkingStatusCode::SelectingProtocol) {
		if (!Packet.contains("d")) {
			return;
		}
		const Json& Data = Packet["d"];
		EncryptionMode_ = Data.value("mode", std::string());

		SecretKey_.clear();
		if (Data.contains("secret_key") && Data["secret_key"].is_array()) {
			for (const Json& Byte : Data["secret_key"]) {
				if (Byte.is_number_integer()) {
					SecretKey_.push_back(static_cast<std::uint8_t>(Byte.get<int>() & 0xFF));
				}
			}
		}

		if (EncryptionMode_ != "aead_xchacha20_poly1305_rtpsize") {
			Fail("Unsupported encryption mode " + EncryptionMode_);
			return;
		}
		if (SecretKey_.size() != 32) {
			Fail("Invalid voice secret key");
			return;
		}

		const int DaveVersion = Data.value("dave_protocol_version", 0);
		DaveProtocolVersion_ = static_cast<std::uint16_t>(std::max(0, DaveVersion));
		if (Debug_ && OnDebug) {
			OnDebug(
				"[NW] encryption " + EncryptionMode_ + " dave="
				+ std::to_string(DaveProtocolVersion_)
			);
		}

		Sequence_ = Random16();
		Timestamp_ = Random32();
		Nonce_ = 0;
		Speaking_ = false;

		SetCode(NetworkingStatusCode::Ready);
		if (DaveProtocolVersion_ > 0) {
			InitializeDave(DaveProtocolVersion_);
		}
		if (OnReady) {
			OnReady();
		}
		return;
	}

	if (Opcode == 11 && Packet.contains("d") && Packet["d"].contains("user_ids")
		&& Packet["d"]["user_ids"].is_array()) {
		for (const Json& Id : Packet["d"]["user_ids"]) {
			if (Id.is_string()) {
				ConnectedClients_.insert(Id.get<std::string>());
			}
		}
		return;
	}

	if (Opcode == 13 && Packet.contains("d") && Packet["d"].contains("user_id")
		&& Packet["d"]["user_id"].is_string()) {
		ConnectedClients_.erase(Packet["d"]["user_id"].get<std::string>());
		return;
	}

	if (Opcode == 21 && Packet.contains("d")) {
		const Json& Data = Packet["d"];
		const std::uint16_t TransitionId = Data.value("transition_id", 0);
		const std::uint16_t Version = Data.value("protocol_version", 0);
		if (Dave_) {
			Dave_->PrepareTransition(TransitionId, Version);
			if (TransitionId != 0) {
				SendTransitionReady(TransitionId);
			}
		}
		return;
	}

	if (Opcode == 22 && Packet.contains("d")) {
		const std::uint16_t TransitionId = Packet["d"].value("transition_id", 0);
		if (Dave_) {
			Dave_->ExecuteTransition(TransitionId);
		}
		return;
	}

	if (Opcode == 24 && Packet.contains("d")) {
		const Json& Data = Packet["d"];
		const std::uint16_t Epoch = Data.value("epoch", 0);
		const std::uint16_t Version = Data.value(
			"protocol_version",
			static_cast<std::uint16_t>(DaveProtocolVersion_)
		);
		if (Epoch == 1 && Version > 0) {
			DaveProtocolVersion_ = Version;
			InitializeDave(Version);
		}
		return;
	}

	if (Opcode == 9 && Code_.load() == NetworkingStatusCode::Resuming) {
		Speaking_ = false;
		SetCode(NetworkingStatusCode::Ready);
		if (OnReady) {
			OnReady();
		}
	}
}

void Networking::HandleBinary(int Opcode, std::vector<std::uint8_t> Payload) {
	if (Opcode == 25) {
		ExternalSender_ = Payload;
		HasExternalSender_ = true;
		if (Dave_ && Dave_->Initialized()) {
			Dave_->SetExternalSender(ExternalSender_);
		}
		return;
	}

	if (!Dave_ || !Dave_->Initialized()) {
		return;
	}

	if (Opcode == 27) {
		std::optional<std::vector<std::uint8_t>> Commit = Dave_->ProcessProposals(
			Payload,
			RecognizedUserIds()
		);
		if (Commit) {
			Ws_->SendBinary(28, *Commit);
		} else if (Debug_ && OnDebug) {
			OnDebug("[DAVE] proposals produced no commit");
		}
		return;
	}

	if (Opcode == 29 || Opcode == 30) {
		if (Payload.size() < 2) {
			return;
		}
		const std::uint16_t TransitionId = static_cast<std::uint16_t>(
			(static_cast<std::uint16_t>(Payload[0]) << 8) | Payload[1]
		);
		const std::span<const std::uint8_t> Body(Payload.data() + 2, Payload.size() - 2);

		const DaveSession::ProcessResult Result = Opcode == 29
			? Dave_->ProcessCommit(TransitionId, Body)
			: Dave_->ProcessWelcome(TransitionId, Body, RecognizedUserIds());

		if (Result.Success) {
			if (TransitionId != 0) {
				SendTransitionReady(TransitionId);
			}
		} else if (!Result.Ignored) {
			SendInvalidCommitWelcome(TransitionId);
		}
	}
}

void Networking::InitializeDave(std::uint16_t Version) {
	if (!Dave_ || !Dave_->Available()) {
		Fail("This channel requires DAVE, but libdave.dll was not found next to the executable");
		return;
	}
	if (Options_.ChannelId.empty()) {
		Fail("Missing channel id for DAVE");
		return;
	}

	std::uint64_t GroupId = 0;
	try {
		GroupId = std::stoull(Options_.ChannelId);
	} catch (const std::exception&) {
		Fail("Invalid channel id for DAVE");
		return;
	}

	DaveGroupId_ = GroupId;
	DaveResets_ = 0;
	if (!Dave_->Init(Version, GroupId, Options_.UserId, Options_.SessionId)) {
		Fail("DAVE session initialization failed");
		return;
	}

	if (HasExternalSender_) {
		Dave_->SetExternalSender(ExternalSender_);
	}
	SendKeyPackage();

	if (Debug_ && OnDebug) {
		OnDebug("[DAVE] session initialized v" + std::to_string(Version));
	}
}

void Networking::SendKeyPackage() {
	if (!Dave_) {
		return;
	}

	std::optional<std::vector<std::uint8_t>> KeyPackage = Dave_->GetKeyPackage();
	if (!KeyPackage) {
		Fail("DAVE key package unavailable");
		return;
	}

	Ws_->SendBinary(26, *KeyPackage);
	if (Debug_ && OnDebug) {
		OnDebug("[DAVE] key package sent (" + std::to_string(KeyPackage->size()) + " bytes)");
	}
}

void Networking::SendTransitionReady(std::uint16_t TransitionId) {
	Json Data;
	Data["transition_id"] = TransitionId;

	Json Packet;
	Packet["op"] = 23;
	Packet["d"] = std::move(Data);

	Ws_->SendPacket(Packet);
}

void Networking::SendInvalidCommitWelcome(std::uint16_t TransitionId) {
	Json Data;
	Data["transition_id"] = TransitionId;

	Json Packet;
	Packet["op"] = 31;
	Packet["d"] = std::move(Data);

	Ws_->SendPacket(Packet);

	if (!Dave_ || !Dave_->Available() || DaveGroupId_ == 0) {
		return;
	}
	if (DaveResets_ >= 3) {
		Fail("DAVE transition failed repeatedly");
		return;
	}

	++DaveResets_;
	Dave_->Reset();
	if (!Dave_->Init(DaveProtocolVersion_, DaveGroupId_, Options_.UserId, Options_.SessionId)) {
		Fail("DAVE session reinitialization failed");
		return;
	}
	if (HasExternalSender_) {
		Dave_->SetExternalSender(ExternalSender_);
	}
	SendKeyPackage();
}

std::vector<std::string> Networking::RecognizedUserIds() const {
	std::vector<std::string> Ids(ConnectedClients_.begin(), ConnectedClients_.end());
	if (!Options_.UserId.empty()
		&& ConnectedClients_.find(Options_.UserId) == ConnectedClients_.end()) {
		Ids.push_back(Options_.UserId);
	}
	return Ids;
}

void Networking::HandleClose(int CloseCode) {
	if (Code_.exchange(NetworkingStatusCode::Closed) == NetworkingStatusCode::Closed) {
		return;
	}
	if (CloseCode == 4017) {
		if (!Dave_ || !Dave_->Available()) {
			Fail("Voice channel requires DAVE/E2EE; libdave.dll was not found");
		} else {
			Fail("Voice channel rejected the DAVE handshake (4017)");
		}
	}
	if (Udp_) {
		Udp_->Destroy();
	}
	if (OnClose) {
		OnClose(CloseCode);
	}
}

void Networking::HandleUdpClose() {
	if (Code_.exchange(NetworkingStatusCode::Closed) == NetworkingStatusCode::Closed) {
		return;
	}
	if (OnClose) {
		OnClose(0);
	}
}

std::optional<OpusPacket> Networking::PrepareAudioPacket(const OpusPacket& Opus) {
	if (Code_.load(std::memory_order_acquire) != NetworkingStatusCode::Ready) {
		return std::nullopt;
	}

	const std::uint16_t PacketSequence = Sequence_++;
	const std::uint32_t PacketTimestamp = Timestamp_;
	Timestamp_ += TimestampIncrement;
	BuildPacket(Opus, PacketSequence, PacketTimestamp);
	return PreparedPacket_;
}

std::optional<OpusPacket> Networking::PrepareAudioPacket(
	const OpusPacket& Opus,
	std::uint16_t Sequence,
	std::uint32_t Timestamp
) {
	if (Code_.load(std::memory_order_acquire) != NetworkingStatusCode::Ready) {
		return std::nullopt;
	}
	BuildPacket(Opus, Sequence, Timestamp);
	return PreparedPacket_;
}

bool Networking::PrepareAudioPacketFast(
	const OpusPacket& Opus,
	std::uint16_t Sequence,
	std::uint32_t Timestamp
) {
	if (Code_.load(std::memory_order_acquire) != NetworkingStatusCode::Ready) {
		return false;
	}
	BuildPacket(Opus, Sequence, Timestamp);
	return true;
}

void Networking::BuildPacket(
	const OpusPacket& Opus,
	std::uint16_t Sequence,
	std::uint32_t Timestamp
) {
	const OpusPacket* Payload = &Opus;
	if (Dave_ && Dave_->Initialized() && Dave_->Encrypt(Opus, Ssrc_, DaveFrame_)) {
		Payload = &DaveFrame_;
	}

	if (Debug_ && OnDebug && !RtpLogged_) {
		RtpLogged_ = true;
		OnDebug(
			"[RTP] ssrc=" + std::to_string(Ssrc_) + " seq=" + std::to_string(Sequence)
			+ " ts=" + std::to_string(Timestamp) + " mode=" + EncryptionMode_
			+ " key=" + std::to_string(SecretKey_.size())
		);
	}

	const std::size_t PlainSize = Payload->size();
	PreparedPacket_.resize(12 + PlainSize + 16 + 4);
	std::uint8_t* Packet = PreparedPacket_.data();

	Packet[0] = 0x80;
	Packet[1] = 0x78;

	const std::uint16_t SequenceBig = Platform::ByteSwap16(Sequence);
	std::memcpy(Packet + 2, &SequenceBig, sizeof(SequenceBig));
	const std::uint32_t TimestampBig = Platform::ByteSwap32(Timestamp);
	std::memcpy(Packet + 4, &TimestampBig, sizeof(TimestampBig));
	const std::uint32_t SsrcBig = Platform::ByteSwap32(Ssrc_);
	std::memcpy(Packet + 8, &SsrcBig, sizeof(SsrcBig));

	std::array<std::uint8_t, 24> Nonce{};
	const std::uint32_t Counter = ++Nonce_;
	Nonce[0] = static_cast<std::uint8_t>((Counter >> 24) & 0xFF);
	Nonce[1] = static_cast<std::uint8_t>((Counter >> 16) & 0xFF);
	Nonce[2] = static_cast<std::uint8_t>((Counter >> 8) & 0xFF);
	Nonce[3] = static_cast<std::uint8_t>(Counter & 0xFF);

	const std::size_t CipherSize = Secretbox::EncryptInto(
		*Payload,
		std::span<const std::uint8_t>(Packet, 12),
		Nonce,
		SecretKey_,
		Packet + 12
	);

	Packet[12 + CipherSize] = Nonce[0];
	Packet[12 + CipherSize + 1] = Nonce[1];
	Packet[12 + CipherSize + 2] = Nonce[2];
	Packet[12 + CipherSize + 3] = Nonce[3];
	PreparedPacket_.resize(12 + CipherSize + 4);
}

bool Networking::DispatchAudio() {
	if (Code_.load(std::memory_order_acquire) != NetworkingStatusCode::Ready) {
		return false;
	}
	if (PreparedPacket_.empty()) {
		return false;
	}

	const std::size_t PacketSize = PreparedPacket_.size();
	const bool Sent = Udp_ && Udp_->Send(PreparedPacket_);
	if (Debug_ && OnDebug && !AudioSendLogged_) {
		AudioSendLogged_ = true;
		OnDebug(
			"[UDP] audio send " + std::to_string(PacketSize) + " bytes ok="
			+ std::to_string(Sent ? 1 : 0)
		);
	}
	PreparedPacket_.clear();
	return Sent;
}

void Networking::SetSpeaking(bool Speaking) {
	if (Code_.load() != NetworkingStatusCode::Ready) {
		return;
	}
	if (Speaking_.exchange(Speaking) == Speaking) {
		return;
	}

	Json Data;
	Data["speaking"] = Speaking ? 1 : 0;
	Data["delay"] = 0;
	Data["ssrc"] = Ssrc_;

	Json Packet;
	Packet["op"] = 5;
	Packet["d"] = std::move(Data);

	Ws_->SendPacket(Packet);
}

bool Networking::DaveReady() const {
	if (DaveProtocolVersion_ == 0 || !Dave_) {
		return DaveProtocolVersion_ == 0;
	}
	if (Dave_->ProtocolVersion() == 0) {
		return true;
	}
	return Dave_->HasKeyRatchet();
}

PingInfo Networking::Ping() const {
	PingInfo Info;
	if (Ws_) {
		Info.WebSocket = Ws_->Ping();
	}
	if (Udp_) {
		Info.Udp = Udp_->Ping();
	}
	return Info;
}

}
