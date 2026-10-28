#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "Voice/Json.h"

namespace Voice {

class VoiceWebSocket {
public:
	VoiceWebSocket(std::string Url, int AckOpcode, int HeartbeatOpcode, bool Debug);
	~VoiceWebSocket();

	VoiceWebSocket(const VoiceWebSocket&) = delete;
	VoiceWebSocket& operator=(const VoiceWebSocket&) = delete;

	void Connect();
	bool SendPacket(const Json& Packet);
	bool SendText(const std::string& Text);
	bool SendBinary(int Opcode, std::span<const std::uint8_t> Payload);
	void SetHeartbeatInterval(int Milliseconds);
	void Close(int Code = 1000);
	void Destroy();
	std::optional<int> Ping() const;
	bool IsOpen() const;

	bool ImmediateHeartbeat = false;
	bool ObjectHeartbeat = false;

	std::function<void()> OnOpen;
	std::function<void(const Json&)> OnPacket;
	std::function<void(int, std::vector<std::uint8_t>)> OnBinary;
	std::function<void(const std::string&)> OnError;
	std::function<void(int)> OnClose;
	std::function<void(const std::string&)> OnDebug;
	std::function<Json()> HeartbeatPayload;

private:
	void OpenLoop();
	bool OpenConnection();
	void CleanupHandles();
	void ReceiveLoop();
	void HeartbeatLoop();
	void SendHeartbeat();
	void HandleMessage(const std::string& Message);
	void Fail(const std::string& Message);
	void JoinThreads();

	std::string Url_;
	int AckOpcode_;
	int HeartbeatOpcode_;
	bool Debug_;

	std::atomic<void*> Session_{nullptr};
	std::atomic<void*> Connection_{nullptr};
	std::atomic<void*> Request_{nullptr};
	std::atomic<void*> Socket_{nullptr};

	std::atomic<bool> Open_{false};
	std::atomic<bool> Stop_{false};
	std::atomic<bool> Started_{false};
	std::atomic<int> HeartbeatInterval_{0};
	std::atomic<std::int64_t> LastHeartbeatAck_{0};
	std::atomic<std::int64_t> LastHeartbeatSend_{0};
	std::atomic<int> MissedHeartbeats_{0};
	std::atomic<int> PingMs_{-1};
	std::atomic<int> CloseCode_{0};
	std::atomic<std::int64_t> LastSequence_{0};

	std::mutex SendMutex_;
	std::thread OpenThread_;
	std::thread HeartbeatThread_;
};

}
