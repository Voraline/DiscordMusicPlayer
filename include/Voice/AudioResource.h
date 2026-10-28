#pragma once

#include <cstddef>
#include <memory>

#include "Voice/Types.h"

namespace Voice {

class AudioResource {
public:
	explicit AudioResource(std::shared_ptr<const EncodedAudio> Audio, int SilencePaddingFrames = 5);

	const OpusPacket* Read();
	bool Readable() const;
	bool Ended() const;
	void Reset();
	std::size_t FrameCount() const;
	std::size_t Cursor() const;
	int SilencePaddingFrames() const;
	const std::shared_ptr<const EncodedAudio>& Audio() const;
	static const OpusPacket& SilenceFrame();

private:
	std::shared_ptr<const EncodedAudio> Audio_;
	std::size_t Cursor_ = 0;
	int SilenceRemaining_ = -1;
	int SilencePaddingFrames_;
};

}
