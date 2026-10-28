#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include "Voice/Types.h"

namespace Voice {

class VoiceUdpSocket {
public:
	VoiceUdpSocket(SocketConfig Remote, bool Debug);
	~VoiceUdpSocket();

	VoiceUdpSocket(const VoiceUdpSocket&) = delete;
	VoiceUdpSocket& operator=(const VoiceUdpSocket&) = delete;

	bool Open();
	bool Send(std::span<const std::uint8_t> Data);
	bool Send(const std::vector<std::uint8_t>& Data);
	std::optional<SocketConfig> PerformIPDiscovery(std::uint32_t Ssrc, std::chrono::milliseconds Timeout);
	void Destroy();
	std::optional<int> Ping() const;
	bool IsOpen() const;

	std::function<void(const std::string&)> OnError;
	std::function<void()> OnClose;
	std::function<void(const std::string&)> OnDebug;
	std::function<void(const std::vector<std::uint8_t>&)> OnMessage;

private:
	struct KeepAlive {
		std::uint32_t Value;
		std::int64_t Timestamp;
	};

	void ReceiveLoop();
	void KeepAliveLoop();
	void HandleDatagram(const std::uint8_t* Data, std::size_t Size);
	void JoinThreads();

	SocketConfig Remote_;
	bool Debug_;
	std::atomic<std::uintptr_t> Socket_{~static_cast<std::uintptr_t>(0)};
	std::atomic<bool> Stop_{false};
	std::atomic<int> PingMs_{-1};
	std::atomic<std::uint32_t> KeepAliveCounter_{0};
	bool KeepAliveWarned_ = false;

	std::mutex Mutex_;
	std::condition_variable DiscoveryCv_;
	std::optional<SocketConfig> DiscoveryResult_;
	std::deque<KeepAlive> KeepAlives_;

	std::thread ReceiveThread_;
	std::thread KeepAliveThread_;
};

}
