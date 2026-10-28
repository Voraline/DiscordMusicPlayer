#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "Voice/Json.h"
#include "Voice/Types.h"

namespace Voice {

class VoiceWebSocket;

class GatewayClient {
public:
	GatewayClient(std::string Token, bool SelfBot, bool Debug);
	~GatewayClient();

	GatewayClient(const GatewayClient&) = delete;
	GatewayClient& operator=(const GatewayClient&) = delete;

	void Connect();
	void Join(const JoinConfig& Config);
	void Leave();
	void Close();
	bool IsReady() const;
	bool IsVoiceReady() const;
	std::string UserId() const;

	std::function<void()> OnReady;
	std::function<void(const VoiceServerInfo&, const VoiceStateInfo&)> OnVoiceReady;
	std::function<void(const std::string&)> OnError;
	std::function<void(const std::string&)> OnDebug;
	std::function<void(int)> OnClose;

private:
	void HandlePacket(const Json& Packet);
	void SendIdentify();
	void SendVoiceState();
	void MaybeNotify();
	void Fail(const std::string& Message);

	std::string Token_;
	bool SelfBot_;
	bool Debug_;
	std::unique_ptr<VoiceWebSocket> Ws_;
	std::atomic<bool> Ready_{false};
	std::atomic<bool> VoiceNotified_{false};
	std::atomic<std::int64_t> Sequence_{-1};

	mutable std::mutex Mutex_;
	JoinConfig Config_;
	std::string UserId_;
	std::string SessionId_;
	VoiceServerInfo Server_;
	VoiceStateInfo State_;
	bool HasServer_ = false;
	bool HasState_ = false;
};

}
