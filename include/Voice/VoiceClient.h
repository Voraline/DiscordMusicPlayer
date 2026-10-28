#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Voice/Types.h"

namespace Voice {

class VoiceClient {
public:
	VoiceClient(std::vector<BotOptions> Bots, JoinConfig Config, PlaybackOptions Options = {});
	~VoiceClient();

	VoiceClient(const VoiceClient&) = delete;
	VoiceClient& operator=(const VoiceClient&) = delete;

	bool Start();
	void PlayFile(const std::filesystem::path& File);
	void Play(std::shared_ptr<EncodedAudio> Audio);
	void Stop();
	void Disconnect();
	bool WaitForFinished();
	bool WaitForReady(std::chrono::milliseconds Timeout);
	std::size_t ReadyCount() const;
	std::size_t BotCount() const;
	std::string LastError() const;

	std::function<void(const std::string&, const std::string&)> OnError;
	std::function<void(const std::string&)> OnDebug;
	std::function<void()> OnFinished;

private:
	struct Impl;
	std::unique_ptr<Impl> Impl_;
};

}
