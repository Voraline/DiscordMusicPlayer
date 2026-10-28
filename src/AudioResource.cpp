#include "Voice/AudioResource.h"

#include <utility>

namespace Voice {

const OpusPacket& AudioResource::SilenceFrame() {
	static const OpusPacket Frame{0xF8, 0xFF, 0xFE};
	return Frame;
}

AudioResource::AudioResource(std::shared_ptr<const EncodedAudio> Audio, int SilencePaddingFrames)
	: Audio_(std::move(Audio)), SilencePaddingFrames_(SilencePaddingFrames) {}

const OpusPacket* AudioResource::Read() {
	if (!Audio_) {
		return nullptr;
	}

	if (SilenceRemaining_ > 0) {
		--SilenceRemaining_;
		return &SilenceFrame();
	}

	if (Cursor_ < Audio_->Frames.size()) {
		return &Audio_->Frames[Cursor_++];
	}

	if (SilenceRemaining_ == -1) {
		SilenceRemaining_ = SilencePaddingFrames_;
		if (SilenceRemaining_ > 0) {
			--SilenceRemaining_;
			return &SilenceFrame();
		}
	}

	return nullptr;
}

bool AudioResource::Readable() const {
	if (!Audio_) {
		return false;
	}
	if (SilenceRemaining_ > 0) {
		return true;
	}
	if (Cursor_ < Audio_->Frames.size()) {
		return true;
	}
	if (SilenceRemaining_ == -1) {
		return SilencePaddingFrames_ > 0;
	}
	return false;
}

bool AudioResource::Ended() const {
	if (!Audio_) {
		return true;
	}
	if (Cursor_ < Audio_->Frames.size()) {
		return false;
	}
	if (SilenceRemaining_ > 0) {
		return false;
	}
	if (SilenceRemaining_ == -1) {
		return SilencePaddingFrames_ == 0;
	}
	return true;
}

void AudioResource::Reset() {
	Cursor_ = 0;
	SilenceRemaining_ = -1;
}

std::size_t AudioResource::FrameCount() const {
	return Audio_ ? Audio_->Frames.size() : 0;
}

std::size_t AudioResource::Cursor() const {
	return Cursor_;
}

int AudioResource::SilencePaddingFrames() const {
	return SilencePaddingFrames_;
}

const std::shared_ptr<const EncodedAudio>& AudioResource::Audio() const {
	return Audio_;
}

}
