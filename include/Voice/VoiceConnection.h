#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Voice/Networking.h"
#include "Voice/Types.h"

namespace Voice {

class AudioPlayer;

class VoiceConnection {
public:
	VoiceConnection(JoinConfig Config, ConnectionOptions Options, bool Debug);
	~VoiceConnection();

	VoiceConnection(const VoiceConnection&) = delete;
	VoiceConnection& operator=(const VoiceConnection&) = delete;

	void Start();
	VoiceConnectionStatus Status() const;
	const JoinConfig& Config() const;
	Networking& Network();
	std::optional<OpusPacket> PrepareAudioPacket(const OpusPacket& Packet, std::uint16_t Sequence, std::uint32_t Timestamp);
	bool PrepareAudioPacketFast(const OpusPacket& Packet, std::uint16_t Sequence, std::uint32_t Timestamp);
	bool DispatchAudio();
	std::optional<OpusPacket> PlayOpusPacket(const OpusPacket& Packet);
	void SetSpeaking(bool Enabled);
	bool DaveReady() const;
	void Subscribe(AudioPlayer* Player);
	void Disconnect();
	void Destroy();
	PingInfo Ping() const;

	std::function<void(VoiceConnectionStatus, VoiceConnectionStatus)> OnStateChange;
	std::function<void()> OnReady;
	std::function<void(int)> OnClose;
	std::function<void(const std::string&)> OnError;
	std::function<void(const std::string&)> OnDebug;

private:
	void SetStatus(VoiceConnectionStatus Next);

	JoinConfig Config_;
	std::atomic<VoiceConnectionStatus> Status_{VoiceConnectionStatus::Signalling};
	std::unique_ptr<Networking> Network_;
	AudioPlayer* Player_ = nullptr;
};

}
