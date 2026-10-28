#pragma once

#include <cstdint>
#include <thread>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace Voice::Platform {

inline void CpuPause() {
#if defined(_MSC_VER)
	_mm_pause();
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	__builtin_ia32_pause();
#else
	std::this_thread::yield();
#endif
}

constexpr std::uint16_t ByteSwap16(std::uint16_t Value) {
	return static_cast<std::uint16_t>((Value >> 8) | (Value << 8));
}

constexpr std::uint32_t ByteSwap32(std::uint32_t Value) {
	return ((Value & 0x000000FFu) << 24)
		| ((Value & 0x0000FF00u) << 8)
		| ((Value & 0x00FF0000u) >> 8)
		| ((Value & 0xFF000000u) >> 24);
}

}
