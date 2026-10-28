#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Voice {

using OpusPacket = std::vector<std::uint8_t>;

enum class VoiceConnectionStatus {
	Signalling,
	Connecting,
	Ready,
	Disconnected,
	Destroyed
};

enum class NetworkingStatusCode {
	OpeningWs,
	Identifying,
	UdpHandshaking,
	SelectingProtocol,
	Ready,
	Resuming,
	Closed
};

enum class AudioPlayerStatus {
	Idle,
	Playing,
	Paused,
	AutoPaused
};

enum class NoSubscriberBehavior {
	Pause,
	Play,
	Stop
};

struct JoinConfig {
	std::string GuildId;
	std::string ChannelId;
	bool SelfDeaf = false;
	bool SelfMute = false;
	std::string Group = "default";
};

struct SocketConfig {
	std::string Ip;
	std::uint16_t Port = 0;
};

struct BotOptions {
	std::string Token;
	std::string Name;
	bool SelfBot = false;
};

struct ConnectionOptions {
	std::string ServerId;
	std::string ChannelId;
	std::string UserId;
	std::string SessionId;
	std::string Token;
	std::string Endpoint;
};

struct VoiceServerInfo {
	std::string Token;
	std::string GuildId;
	std::string Endpoint;
};

struct VoiceStateInfo {
	std::string SessionId;
	std::string ChannelId;
	std::string UserId;
	std::string GuildId;
};

struct PingInfo {
	std::optional<int> WebSocket;
	std::optional<int> Udp;
};

struct EncoderOptions {
	std::uint32_t Bitrate = 128000;
	int Complexity = 10;
	std::string FfmpegPath = "ffmpeg";
	bool Debug = false;
};

struct PlaybackOptions {
	std::chrono::milliseconds LeadTime{2000};
	std::chrono::milliseconds ConnectTimeout{60000};
	std::chrono::milliseconds ConnectStagger{750};
	int SilencePaddingFrames = 5;
	std::uint32_t Bitrate = 128000;
	int OpusComplexity = 10;
	std::string FfmpegPath = "ffmpeg";
	bool Debug = false;
};

struct EncodedAudio {
	std::vector<OpusPacket> Frames;
	int SampleRate = 48000;
	int Channels = 2;
	int FrameDurationMs = 20;
};

}
