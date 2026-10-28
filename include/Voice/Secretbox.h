#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Voice::Secretbox {

void Init();

std::vector<std::uint8_t> EncryptXChaCha20Poly1305(
	std::span<const std::uint8_t> Plaintext,
	std::span<const std::uint8_t> AdditionalData,
	std::span<const std::uint8_t> Nonce,
	std::span<const std::uint8_t> Key
);

std::size_t EncryptInto(
	std::span<const std::uint8_t> Plaintext,
	std::span<const std::uint8_t> AdditionalData,
	std::span<const std::uint8_t> Nonce,
	std::span<const std::uint8_t> Key,
	std::uint8_t* Out
);

void RandomBytes(std::span<std::uint8_t> Buffer);

}
