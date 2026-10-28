#pragma once

#include <filesystem>
#include <memory>

#include "Voice/Types.h"

namespace Voice {

class OpusCodec {
public:
	static std::shared_ptr<EncodedAudio> EncodeFile(const std::filesystem::path& File, const EncoderOptions& Options);
};

}
