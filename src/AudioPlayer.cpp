#include "Voice/AudioPlayer.h"

#include <algorithm>
#include <string>
#include <utility>

#include "Voice/VoiceConnection.h"

namespace Voice {

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
	ClearSubscribers();
}

void AudioPlayer::Play(std::shared_ptr<AudioResource> Resource) {
	{
		std::lock_guard Lock(Mutex_);
		Resource_ = std::move(Resource);
		MissedFrames_ = 0;
		PlaybackDuration_ = 0;
		DrainRemaining_ = 0;
		StopRequested_ = false;
	}
	SetStatus(AudioPlayerStatus::Playing);
}

bool AudioPlayer::Pause(bool InterpolateSilence) {
	(void)InterpolateSilence;
	if (Status_.load() != AudioPlayerStatus::Playing) {
		return false;
	}
	SetStatus(AudioPlayerStatus::Paused);
	return true;
}

bool AudioPlayer::Unpause() {
	if (Status_.load() != AudioPlayerStatus::Paused) {
		return false;
	}
	MissedFrames_ = 0;
	SetStatus(AudioPlayerStatus::Playing);
	return true;
}

bool AudioPlayer::Stop(bool Force) {
	if (Status_.load() == AudioPlayerStatus::Idle) {
		return false;
	}

	std::shared_ptr<AudioResource> Resource;
	{
		std::lock_guard Lock(Mutex_);
		Resource = Resource_;
	}

	if (Force || !Resource || Resource->SilencePaddingFrames() == 0) {
		SetIdle();
		return true;
	}

	{
		std::lock_guard Lock(Mutex_);
		StopRequested_ = true;
		DrainRemaining_ = Resource->SilencePaddingFrames();
	}
	return true;
}

void AudioPlayer::StepPrepare(std::uint16_t Sequence, std::uint32_t Timestamp) {
	AudioPlayerStatus Status = Status_.load();
	if (Status == AudioPlayerStatus::Idle) {
		return;
	}

	VoiceConnection* Single = SingleSubscriber_.load(std::memory_order_acquire);
	std::shared_ptr<AudioResource> Resource;
	std::vector<VoiceConnection*> Subscribers;
	bool StopRequested = false;
	int DrainRemaining = 0;
	{
		std::lock_guard Lock(Mutex_);
		Resource = Resource_;
		if (!Single) {
			Subscribers = Subscribers_;
		}
		StopRequested = StopRequested_;
		DrainRemaining = DrainRemaining_;
		if (StopRequested && DrainRemaining > 0) {
			DrainRemaining_ = DrainRemaining - 1;
		}
	}

	if (!Resource) {
		SetIdle();
		return;
	}

	std::size_t Playable = 0;
	if (Single) {
		if (Single->Status() == VoiceConnectionStatus::Ready) {
			Playable = 1;
		}
	} else {
		for (VoiceConnection* Connection : Subscribers) {
			if (Connection && Connection->Status() == VoiceConnectionStatus::Ready) {
				++Playable;
			}
		}
	}

	auto Prepare = [&](const OpusPacket& Packet) {
		if (Single) {
			if (Single->Status() == VoiceConnectionStatus::Ready) {
				Single->PrepareAudioPacketFast(Packet, Sequence, Timestamp);
			}
			return;
		}
		for (VoiceConnection* Connection : Subscribers) {
			if (Connection && Connection->Status() == VoiceConnectionStatus::Ready) {
				Connection->PrepareAudioPacketFast(Packet, Sequence, Timestamp);
			}
		}
	};

	if (Status == AudioPlayerStatus::Paused) {
		return;
	}

	if (Status == AudioPlayerStatus::AutoPaused) {
		if (Playable == 0) {
			return;
		}
		MissedFrames_ = 0;
		SetStatus(AudioPlayerStatus::Playing);
		Status = AudioPlayerStatus::Playing;
	}

	if (Playable == 0) {
		if (NoSubscriber_ == NoSubscriberBehavior::Pause) {
			SetStatus(AudioPlayerStatus::AutoPaused);
			return;
		}
		if (NoSubscriber_ == NoSubscriberBehavior::Stop) {
			Stop(true);
			return;
		}
	}

	if (StopRequested) {
		if (DrainRemaining > 0) {
			Prepare(AudioResource::SilenceFrame());
			return;
		}
		SetIdle();
		return;
	}

	const OpusPacket* Packet = Resource->Read();
	if (!Packet) {
		++MissedFrames_;
		if (MissedFrames_ >= MaxMissedFrames_) {
			SetIdle();
			return;
		}
		Packet = &AudioResource::SilenceFrame();
	} else {
		MissedFrames_ = 0;
	}

	PlaybackDuration_ += 20;
	Prepare(*Packet);
}

void AudioPlayer::StepDispatch() {
	if (Status_.load() == AudioPlayerStatus::Idle) {
		return;
	}

	VoiceConnection* Single = SingleSubscriber_.load(std::memory_order_acquire);
	if (Single) {
		if (Single->Status() == VoiceConnectionStatus::Ready) {
			Single->DispatchAudio();
		}
		return;
	}

	std::vector<VoiceConnection*> Subscribers;
	{
		std::lock_guard Lock(Mutex_);
		Subscribers = Subscribers_;
	}

	for (VoiceConnection* Connection : Subscribers) {
		if (Connection && Connection->Status() == VoiceConnectionStatus::Ready) {
			Connection->DispatchAudio();
		}
	}
}

void AudioPlayer::Subscribe(VoiceConnection* Connection) {
	if (!Connection) {
		return;
	}
	std::lock_guard Lock(Mutex_);
	if (std::find(Subscribers_.begin(), Subscribers_.end(), Connection) == Subscribers_.end()) {
		Subscribers_.push_back(Connection);
		SingleSubscriber_.store(
			Subscribers_.size() == 1 ? Connection : nullptr,
			std::memory_order_release
		);
	}
}

void AudioPlayer::Unsubscribe(VoiceConnection* Connection) {
	bool Removed = false;
	{
		std::lock_guard Lock(Mutex_);
		const auto Found = std::find(Subscribers_.begin(), Subscribers_.end(), Connection);
		if (Found != Subscribers_.end()) {
			Subscribers_.erase(Found);
			SingleSubscriber_.store(
				Subscribers_.size() == 1 ? Subscribers_.front() : nullptr,
				std::memory_order_release
			);
			Removed = true;
		}
	}
	if (Removed && Connection) {
		Connection->SetSpeaking(false);
	}
}

void AudioPlayer::ClearSubscribers() {
	std::lock_guard Lock(Mutex_);
	Subscribers_.clear();
	SingleSubscriber_.store(nullptr, std::memory_order_release);
}

void AudioPlayer::SetNoSubscriberBehavior(NoSubscriberBehavior Behavior) {
	NoSubscriber_ = Behavior;
}

void AudioPlayer::SetMaxMissedFrames(int Frames) {
	MaxMissedFrames_ = Frames;
}

AudioPlayerStatus AudioPlayer::Status() const {
	return Status_.load();
}

std::size_t AudioPlayer::PlayableCount() const {
	VoiceConnection* Single = SingleSubscriber_.load(std::memory_order_acquire);
	if (Single) {
		return Single->Status() == VoiceConnectionStatus::Ready ? 1 : 0;
	}

	std::lock_guard Lock(Mutex_);
	std::size_t Count = 0;
	for (VoiceConnection* Connection : Subscribers_) {
		if (Connection && Connection->Status() == VoiceConnectionStatus::Ready) {
			++Count;
		}
	}
	return Count;
}

std::shared_ptr<AudioResource> AudioPlayer::Resource() const {
	std::lock_guard Lock(Mutex_);
	return Resource_;
}

int AudioPlayer::PlaybackDuration() const {
	return PlaybackDuration_.load();
}

void AudioPlayer::SetStatus(AudioPlayerStatus Next) {
	const AudioPlayerStatus Previous = Status_.exchange(Next);
	if (Previous == Next) {
		return;
	}
	if (OnStateChange) {
		OnStateChange(Previous, Next);
	}
	if (OnDebug) {
		OnDebug(
			"player state " + std::to_string(static_cast<int>(Previous)) + " -> "
			+ std::to_string(static_cast<int>(Next))
		);
	}
}

void AudioPlayer::SetIdle() {
	{
		std::lock_guard Lock(Mutex_);
		StopRequested_ = false;
		DrainRemaining_ = 0;
	}
	SetStatus(AudioPlayerStatus::Idle);
}

}
