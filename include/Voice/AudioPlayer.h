#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Voice/AudioResource.h"
#include "Voice/Types.h"

namespace Voice {

class VoiceConnection;

class AudioPlayer {
public:
	AudioPlayer();
	~AudioPlayer();

	void Play(std::shared_ptr<AudioResource> Resource);
	bool Pause(bool InterpolateSilence = true);
	bool Unpause();
	bool Stop(bool Force = false);
	void StepPrepare(std::uint16_t Sequence, std::uint32_t Timestamp);
	void StepDispatch();
	void Subscribe(VoiceConnection* Connection);
	void Unsubscribe(VoiceConnection* Connection);
	void ClearSubscribers();
	void SetNoSubscriberBehavior(NoSubscriberBehavior Behavior);
	void SetMaxMissedFrames(int Frames);
	AudioPlayerStatus Status() const;
	std::size_t PlayableCount() const;
	std::shared_ptr<AudioResource> Resource() const;
	int PlaybackDuration() const;

	std::function<void(AudioPlayerStatus, AudioPlayerStatus)> OnStateChange;
	std::function<void(const std::string&)> OnDebug;

private:
	void SetStatus(AudioPlayerStatus Next);
	void SetIdle();

	std::atomic<AudioPlayerStatus> Status_{AudioPlayerStatus::Idle};
	mutable std::mutex Mutex_;
	std::shared_ptr<AudioResource> Resource_;
	std::vector<VoiceConnection*> Subscribers_;
	std::atomic<VoiceConnection*> SingleSubscriber_{nullptr};
	NoSubscriberBehavior NoSubscriber_ = NoSubscriberBehavior::Pause;
	int MaxMissedFrames_ = 5;
	int MissedFrames_ = 0;
	std::atomic<int> PlaybackDuration_{0};
	int DrainRemaining_ = 0;
	bool StopRequested_ = false;
};

}
