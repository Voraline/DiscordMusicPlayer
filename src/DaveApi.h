#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Voice::DaveApi {

using Handle = void*;
using FailureCallback = void (*)(const char*, const char*, void*);

struct Functions {
	std::uint16_t (*MaxProtocolVersion)() = nullptr;
	void (*Free)(void*) = nullptr;

	Handle (*SessionCreate)(void*, const char*, FailureCallback, void*) = nullptr;
	void (*SessionDestroy)(Handle) = nullptr;
	void (*SessionInit)(Handle, std::uint16_t, std::uint64_t, const char*) = nullptr;
	void (*SessionReset)(Handle) = nullptr;
	void (*SessionSetProtocolVersion)(Handle, std::uint16_t) = nullptr;
	void (*SessionSetExternalSender)(Handle, const std::uint8_t*, std::size_t) = nullptr;
	void (*SessionProcessProposals)(
		Handle,
		const std::uint8_t*,
		std::size_t,
		const char**,
		std::size_t,
		std::uint8_t**,
		std::size_t*
	) = nullptr;
	Handle (*SessionProcessCommit)(Handle, const std::uint8_t*, std::size_t) = nullptr;
	Handle (*SessionProcessWelcome)(
		Handle,
		const std::uint8_t*,
		std::size_t,
		const char**,
		std::size_t
	) = nullptr;
	void (*SessionGetKeyPackage)(Handle, std::uint8_t**, std::size_t*) = nullptr;
	Handle (*SessionGetKeyRatchet)(Handle, const char*) = nullptr;

	void (*KeyRatchetDestroy)(Handle) = nullptr;

	bool (*CommitIsFailed)(Handle) = nullptr;
	bool (*CommitIsIgnored)(Handle) = nullptr;
	void (*CommitDestroy)(Handle) = nullptr;
	void (*WelcomeDestroy)(Handle) = nullptr;

	Handle (*EncryptorCreate)() = nullptr;
	void (*EncryptorDestroy)(Handle) = nullptr;
	void (*EncryptorSetKeyRatchet)(Handle, Handle) = nullptr;
	void (*EncryptorSetPassthrough)(Handle, bool) = nullptr;
	void (*EncryptorAssignSsrc)(Handle, std::uint32_t, int) = nullptr;
	std::size_t (*EncryptorMaxCiphertext)(Handle, int, std::size_t) = nullptr;
	int (*EncryptorEncrypt)(
		Handle,
		int,
		std::uint32_t,
		const std::uint8_t*,
		std::size_t,
		std::uint8_t*,
		std::size_t,
		std::size_t*
	) = nullptr;
	bool (*EncryptorHasKeyRatchet)(Handle) = nullptr;
	bool (*EncryptorIsPassthroughMode)(Handle) = nullptr;
	std::uint16_t (*EncryptorGetProtocolVersion)(Handle) = nullptr;
	void (*SetLogSink)(void (*)(int, const char*, int, const char*)) = nullptr;
};

std::shared_ptr<Functions> Load();

}
