#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Voice {

class DaveSession {
public:
	DaveSession();
	~DaveSession();

	DaveSession(const DaveSession&) = delete;
	DaveSession& operator=(const DaveSession&) = delete;

	struct ProcessResult {
		bool Success = false;
		bool Ignored = false;
	};

	bool Available() const;
	std::uint16_t MaxProtocolVersion() const;
	bool Initialized() const;
	std::uint16_t ProtocolVersion() const;
	bool HasKeyRatchet() const;

	bool Init(
		std::uint16_t Version,
		std::uint64_t GroupId,
		const std::string& SelfUserId,
		const std::string& AuthSessionId
	);
	void Reset();
	void SetExternalSender(std::span<const std::uint8_t> ExternalSender);
	std::optional<std::vector<std::uint8_t>> GetKeyPackage();
	std::optional<std::vector<std::uint8_t>> ProcessProposals(
		std::span<const std::uint8_t> Payload,
		const std::vector<std::string>& RecognizedUserIds
	);
	ProcessResult ProcessCommit(std::uint16_t TransitionId, std::span<const std::uint8_t> Commit);
	ProcessResult ProcessWelcome(
		std::uint16_t TransitionId,
		std::span<const std::uint8_t> Welcome,
		const std::vector<std::string>& RecognizedUserIds
	);
	void PrepareTransition(std::uint16_t TransitionId, std::uint16_t Version);
	bool ExecuteTransition(std::uint16_t TransitionId);
	bool Encrypt(std::span<const std::uint8_t> Opus, std::uint32_t Ssrc, std::vector<std::uint8_t>& Out);

	std::function<void(const std::string&)> OnFailure;
	std::function<void(const std::string&)> OnDebug;

private:
	struct Api;
	static void FailureCallback(const char* Source, const char* Reason, void* UserData);
	void ApplyRatchet(void* Ratchet);
	void StoreRatchet(std::uint16_t TransitionId, void* Ratchet);
	void ApplyPending(std::uint16_t TransitionId);
	void ClearPending();
	void SetPassthrough(bool Enabled);
	void CaptureRatchet(std::uint16_t TransitionId);
	void ResetInternal();

	std::shared_ptr<Api> Api_;
	void* Session_ = nullptr;
	void* Encryptor_ = nullptr;
	std::string SelfUserId_;
	std::uint16_t ProtocolVersion_ = 0;
	bool Initialized_ = false;
	std::uint32_t AssignedSsrc_ = 0;
	bool HasAssignedSsrc_ = false;

	std::vector<std::uint8_t> ExternalSender_;
	bool HasExternalSender_ = false;

	std::unordered_map<std::uint16_t, std::uint16_t> PendingVersions_;
	std::unordered_map<std::uint16_t, void*> PendingRatchets_;
	bool EncryptLogged_ = false;
	bool FrameLogged_ = false;
	mutable std::mutex Mutex_;
};

}
