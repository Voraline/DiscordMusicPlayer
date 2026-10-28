#include "Voice/Secretbox.h"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#include <array>
#include <cstddef>
#include <stdexcept>

#include "monocypher.h"

namespace Voice::Secretbox {

namespace {

constexpr std::size_t NonceSize = 24;
constexpr std::size_t KeySize = 32;
constexpr std::size_t MacSize = 16;

}

void Init() {}

std::size_t EncryptInto(
	std::span<const std::uint8_t> Plaintext,
	std::span<const std::uint8_t> AdditionalData,
	std::span<const std::uint8_t> Nonce,
	std::span<const std::uint8_t> Key,
	std::uint8_t* Out
) {
	if (Nonce.size() != NonceSize) {
		throw std::runtime_error("Invalid XChaCha20-Poly1305 nonce size");
	}
	if (Key.size() != KeySize) {
		throw std::runtime_error("Invalid XChaCha20-Poly1305 key size");
	}

	const std::uint8_t Zero = 0;
	const std::uint8_t* Plain = Plaintext.empty() ? &Zero : Plaintext.data();
	const std::uint8_t* Associated = AdditionalData.empty() ? nullptr : AdditionalData.data();

	crypto_aead_lock(
		Out,
		Out + Plaintext.size(),
		Key.data(),
		Nonce.data(),
		Associated,
		AdditionalData.size(),
		Plain,
		Plaintext.size()
	);

	return Plaintext.size() + MacSize;
}

std::vector<std::uint8_t> EncryptXChaCha20Poly1305(
	std::span<const std::uint8_t> Plaintext,
	std::span<const std::uint8_t> AdditionalData,
	std::span<const std::uint8_t> Nonce,
	std::span<const std::uint8_t> Key
) {
	std::vector<std::uint8_t> Ciphertext(Plaintext.size() + MacSize);
	EncryptInto(Plaintext, AdditionalData, Nonce, Key, Ciphertext.data());
	return Ciphertext;
}

void RandomBytes(std::span<std::uint8_t> Buffer) {
	if (Buffer.empty()) {
		return;
	}

#if defined(_WIN32)
	const NTSTATUS Status = BCryptGenRandom(
		nullptr,
		Buffer.data(),
		static_cast<ULONG>(Buffer.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG
	);
	if (Status != 0) {
		throw std::runtime_error("BCryptGenRandom failed");
	}
#else
	std::size_t Offset = 0;
#if defined(__linux__)
	while (Offset < Buffer.size()) {
		const ssize_t Result = getrandom(Buffer.data() + Offset, Buffer.size() - Offset, 0);
		if (Result > 0) {
			Offset += static_cast<std::size_t>(Result);
			continue;
		}
		if (Result < 0 && errno == EINTR) {
			continue;
		}
		break;
	}
#endif
	if (Offset < Buffer.size()) {
		const int Descriptor = open("/dev/urandom", O_RDONLY);
		if (Descriptor < 0) {
			throw std::runtime_error("Failed to open /dev/urandom");
		}
		while (Offset < Buffer.size()) {
			const ssize_t Result = read(Descriptor, Buffer.data() + Offset, Buffer.size() - Offset);
			if (Result > 0) {
				Offset += static_cast<std::size_t>(Result);
				continue;
			}
			if (Result < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		close(Descriptor);
	}
	if (Offset != Buffer.size()) {
		throw std::runtime_error("Failed to generate random bytes");
	}
#endif
}

}
