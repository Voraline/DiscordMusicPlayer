#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Voice/Json.h"
#include "Voice/Types.h"

namespace Voice {

class VoiceWebSocket;
class VoiceUdpSocket;
class DaveSession;

class Networking {
public:
	Networking(ConnectionOptions Options, bool Debug);
	~Networking();

	Networking(const Networking&) = delete;
	Networking& operator=(const Networking&) = delete;

	void Start();
	void Destroy();
	NetworkingStatusCode Code() const;
	std::optional<OpusPacket> PrepareAudioPacket(const OpusPacket& Opus);
	std::optional<OpusPacket> PrepareAudioPacket(const OpusPacket& Opus, std::uint16_t Sequence, std::uint32_t Timestamp);
	bool PrepareAudioPacketFast(const OpusPacket& Opus, std::uint16_t Sequence, std::uint32_t Timestamp);
	bool DispatchAudio();
	void SetSpeaking(bool Speaking);
	bool DaveReady() const;
	PingInfo Ping() const;

	std::function<void()> OnReady;
	std::function<void(int)> OnClose;
	std::function<void(const std::string&)> OnError;
	std::function<void(const std::string&)> OnDebug;
	std::function<void(NetworkingStatusCode, NetworkingStatusCode)> OnStateChange;

private:
	void HandlePacket(const Json& Packet);
	void HandleBinary(int Opcode, std::vector<std::uint8_t> Payload);
	void HandleClose(int Code);
	void HandleUdpClose();
	void SendIdentify();
	void InitializeDave(std::uint16_t Version);
	void SendKeyPackage();
	void SendTransitionReady(std::uint16_t TransitionId);
	void SendInvalidCommitWelcome(std::uint16_t TransitionId);
	std::vector<std::string> RecognizedUserIds() const;
	void SetCode(NetworkingStatusCode Next);
	void Fail(const std::string& Message);
	std::optional<std::string> ChooseEncryptionMode(const std::vector<std::string>& Modes) const;
	void BuildPacket(const OpusPacket& Opus, std::uint16_t Sequence, std::uint32_t Timestamp);

	ConnectionOptions Options_;
	bool Debug_;
	std::atomic<NetworkingStatusCode> Code_{NetworkingStatusCode::OpeningWs};
	std::unique_ptr<VoiceWebSocket> Ws_;
	std::unique_ptr<VoiceUdpSocket> Udp_;
	std::unique_ptr<DaveSession> Dave_;
	std::jthread DiscoveryThread_;

	std::uint32_t Ssrc_ = 0;
	std::string EncryptionMode_;
	std::vector<std::uint8_t> SecretKey_;
	std::uint16_t Sequence_ = 0;
	std::uint32_t Timestamp_ = 0;
	std::uint32_t Nonce_ = 0;
	std::atomic<bool> Speaking_{false};
	bool RtpLogged_ = false;
	bool AudioSendLogged_ = false;
	OpusPacket PreparedPacket_;
	OpusPacket DaveFrame_;

	std::uint16_t DaveProtocolVersion_ = 0;
	std::uint64_t DaveGroupId_ = 0;
	std::vector<std::uint8_t> ExternalSender_;
	bool HasExternalSender_ = false;
	int DaveResets_ = 0;
	std::set<std::string> ConnectedClients_;
};

}
