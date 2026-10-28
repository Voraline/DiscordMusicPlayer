#include "DaveSession.h"

#include <algorithm>
#include <string>
#include <utility>

#include "DaveApi.h"

namespace Voice {

struct DaveSession::Api {
	std::shared_ptr<DaveApi::Functions> Fn;
};

DaveSession::DaveSession() {
	static const std::shared_ptr<Api> SharedApi = [] {
		auto FunctionsPtr = DaveApi::Load();
		if (!FunctionsPtr) {
			return std::shared_ptr<Api>();
		}
		auto Result = std::make_shared<Api>();
		Result->Fn = std::move(FunctionsPtr);
		return Result;
	}();

	Api_ = SharedApi;
	if (Api_ && Api_->Fn->EncryptorCreate) {
		Encryptor_ = Api_->Fn->EncryptorCreate();
		SetPassthrough(true);
	}
}

DaveSession::~DaveSession() {
	std::lock_guard Lock(Mutex_);
	ClearPending();
	if (Session_ && Api_) {
		Api_->Fn->SessionDestroy(Session_);
		Session_ = nullptr;
	}
	if (Encryptor_ && Api_) {
		Api_->Fn->EncryptorDestroy(Encryptor_);
		Encryptor_ = nullptr;
	}
}

bool DaveSession::Available() const {
	return Api_ && Api_->Fn && Api_->Fn->MaxProtocolVersion;
}

std::uint16_t DaveSession::MaxProtocolVersion() const {
	if (!Available()) {
		return 0;
	}
	return Api_->Fn->MaxProtocolVersion();
}

bool DaveSession::Initialized() const {
	std::lock_guard Lock(Mutex_);
	return Initialized_ && Session_ != nullptr;
}

std::uint16_t DaveSession::ProtocolVersion() const {
	std::lock_guard Lock(Mutex_);
	return ProtocolVersion_;
}

bool DaveSession::HasKeyRatchet() const {
	std::lock_guard Lock(Mutex_);
	if (!Api_ || !Encryptor_) {
		return false;
	}
	return Api_->Fn->EncryptorHasKeyRatchet(Encryptor_);
}

bool DaveSession::Init(
	std::uint16_t Version,
	std::uint64_t GroupId,
	const std::string& SelfUserId,
	const std::string& AuthSessionId
) {
	std::lock_guard Lock(Mutex_);
	if (!Api_ || !Api_->Fn->SessionCreate) {
		return false;
	}

	ResetInternal();
	SelfUserId_ = SelfUserId;

	Session_ = Api_->Fn->SessionCreate(
		nullptr,
		AuthSessionId.empty() ? nullptr : AuthSessionId.c_str(),
		&DaveSession::FailureCallback,
		this
	);
	if (!Session_) {
		return false;
	}

	if (HasExternalSender_) {
		Api_->Fn->SessionSetExternalSender(Session_, ExternalSender_.data(), ExternalSender_.size());
	}

	Api_->Fn->SessionInit(Session_, Version, GroupId, SelfUserId_.c_str());
	ProtocolVersion_ = Version;
	Initialized_ = true;

	SetPassthrough(true);
	if (OnDebug) {
		OnDebug("session initialized v" + std::to_string(Version) + " group " + std::to_string(GroupId));
	}
	return true;
}

void DaveSession::Reset() {
	std::lock_guard Lock(Mutex_);
	ResetInternal();
}

void DaveSession::ResetInternal() {
	ClearPending();
	if (Session_ && Api_) {
		Api_->Fn->SessionDestroy(Session_);
		Session_ = nullptr;
	}
	if (Encryptor_ && Api_) {
		Api_->Fn->EncryptorDestroy(Encryptor_);
		Encryptor_ = nullptr;
	}
	Initialized_ = false;
	ProtocolVersion_ = 0;
	AssignedSsrc_ = 0;
	HasAssignedSsrc_ = false;
	EncryptLogged_ = false;
	FrameLogged_ = false;

	if (Api_ && Api_->Fn->EncryptorCreate) {
		Encryptor_ = Api_->Fn->EncryptorCreate();
		SetPassthrough(true);
	}
}

void DaveSession::SetExternalSender(std::span<const std::uint8_t> ExternalSender) {
	std::lock_guard Lock(Mutex_);
	ExternalSender_.assign(ExternalSender.begin(), ExternalSender.end());
	HasExternalSender_ = true;
	if (Session_ && Api_) {
		Api_->Fn->SessionSetExternalSender(
			Session_,
			ExternalSender_.data(),
			ExternalSender_.size()
		);
	}
	if (OnDebug) {
		OnDebug("external sender set (" + std::to_string(ExternalSender_.size()) + " bytes)");
	}
}

std::optional<std::vector<std::uint8_t>> DaveSession::GetKeyPackage() {
	std::lock_guard Lock(Mutex_);
	if (!Session_ || !Api_) {
		return std::nullopt;
	}

	std::uint8_t* Data = nullptr;
	std::size_t Length = 0;
	Api_->Fn->SessionGetKeyPackage(Session_, &Data, &Length);
	if (!Data) {
		return std::nullopt;
	}

	std::vector<std::uint8_t> Result(Data, Data + Length);
	Api_->Fn->Free(Data);
	return Result;
}

std::optional<std::vector<std::uint8_t>> DaveSession::ProcessProposals(
	std::span<const std::uint8_t> Payload,
	const std::vector<std::string>& RecognizedUserIds
) {
	std::lock_guard Lock(Mutex_);
	if (!Session_ || !Api_ || Payload.empty()) {
		return std::nullopt;
	}

	std::vector<const char*> Ids;
	Ids.reserve(RecognizedUserIds.size());
	for (const std::string& Id : RecognizedUserIds) {
		Ids.push_back(Id.c_str());
	}
	const char* Dummy = "";
	const char** IdsPtr = Ids.empty() ? &Dummy : Ids.data();

	std::uint8_t* Data = nullptr;
	std::size_t Length = 0;
	Api_->Fn->SessionProcessProposals(
		Session_,
		Payload.data(),
		Payload.size(),
		IdsPtr,
		Ids.size(),
		&Data,
		&Length
	);
	if (!Data) {
		return std::nullopt;
	}

	std::vector<std::uint8_t> Result(Data, Data + Length);
	Api_->Fn->Free(Data);
	if (OnDebug) {
		OnDebug("proposals processed, commit/welcome " + std::to_string(Result.size()) + " bytes");
	}
	return Result;
}

DaveSession::ProcessResult DaveSession::ProcessCommit(
	std::uint16_t TransitionId,
	std::span<const std::uint8_t> Commit
) {
	std::lock_guard Lock(Mutex_);
	if (!Session_ || !Api_ || Commit.empty()) {
		return {};
	}

	void* Result = Api_->Fn->SessionProcessCommit(Session_, Commit.data(), Commit.size());
	if (!Result) {
		return {};
	}

	const bool Failed = Api_->Fn->CommitIsFailed(Result);
	const bool Ignored = Api_->Fn->CommitIsIgnored(Result);
	Api_->Fn->CommitDestroy(Result);

	if (Failed) {
		return {};
	}
	if (Ignored) {
		return {false, true};
	}

	CaptureRatchet(TransitionId);
	if (TransitionId != 0) {
		PendingVersions_[TransitionId] = ProtocolVersion_;
	}
	if (OnDebug) {
		OnDebug("commit processed, transition " + std::to_string(TransitionId));
	}
	return {true, false};
}

DaveSession::ProcessResult DaveSession::ProcessWelcome(
	std::uint16_t TransitionId,
	std::span<const std::uint8_t> Welcome,
	const std::vector<std::string>& RecognizedUserIds
) {
	std::lock_guard Lock(Mutex_);
	if (!Session_ || !Api_ || Welcome.empty()) {
		return {};
	}

	std::vector<const char*> Ids;
	Ids.reserve(RecognizedUserIds.size());
	for (const std::string& Id : RecognizedUserIds) {
		Ids.push_back(Id.c_str());
	}
	const char* Dummy = "";
	const char** IdsPtr = Ids.empty() ? &Dummy : Ids.data();

	void* Result = Api_->Fn->SessionProcessWelcome(
		Session_,
		Welcome.data(),
		Welcome.size(),
		IdsPtr,
		Ids.size()
	);
	if (!Result) {
		return {};
	}

	Api_->Fn->WelcomeDestroy(Result);
	CaptureRatchet(TransitionId);
	if (TransitionId != 0) {
		PendingVersions_[TransitionId] = ProtocolVersion_;
	}
	if (OnDebug) {
		OnDebug("welcome processed, transition " + std::to_string(TransitionId));
	}
	return {true, false};
}

void DaveSession::PrepareTransition(std::uint16_t TransitionId, std::uint16_t Version) {
	std::lock_guard Lock(Mutex_);
	PendingVersions_[TransitionId] = Version;

	if (Version == 0) {
		SetPassthrough(true);
	}

	if (TransitionId == 0) {
		ProtocolVersion_ = Version;
		PendingVersions_.erase(TransitionId);
		ApplyPending(TransitionId);
	}
}

bool DaveSession::ExecuteTransition(std::uint16_t TransitionId) {
	std::lock_guard Lock(Mutex_);
	const auto Found = PendingVersions_.find(TransitionId);
	if (Found == PendingVersions_.end()) {
		return false;
	}

	ProtocolVersion_ = Found->second;
	PendingVersions_.erase(Found);
	ApplyPending(TransitionId);

	if (ProtocolVersion_ == 0) {
		SetPassthrough(true);
	}
	if (OnDebug) {
		OnDebug("transition " + std::to_string(TransitionId) + " executed");
	}
	return true;
}

bool DaveSession::Encrypt(
	std::span<const std::uint8_t> Opus,
	std::uint32_t Ssrc,
	std::vector<std::uint8_t>& Out
) {
	std::lock_guard Lock(Mutex_);
	Out.clear();

	if (!Api_ || !Encryptor_ || ProtocolVersion_ == 0) {
		return false;
	}
	if (Opus.size() == 3 && Opus[0] == 0xF8 && Opus[1] == 0xFF && Opus[2] == 0xFE) {
		return false;
	}
	const bool HasRatchet = Api_->Fn->EncryptorHasKeyRatchet(Encryptor_);
	if (OnDebug && !EncryptLogged_) {
		EncryptLogged_ = true;
		const bool Passthrough = Api_->Fn->EncryptorIsPassthroughMode
			? Api_->Fn->EncryptorIsPassthroughMode(Encryptor_)
			: false;
		const int Version = Api_->Fn->EncryptorGetProtocolVersion
			? static_cast<int>(Api_->Fn->EncryptorGetProtocolVersion(Encryptor_))
			: -1;
		OnDebug(
			"encrypt state ratchet=" + std::to_string(HasRatchet ? 1 : 0)
			+ " passthrough=" + std::to_string(Passthrough ? 1 : 0)
			+ " version=" + std::to_string(Version)
		);
	}
	if (!HasRatchet) {
		if (OnDebug) {
			OnDebug("encrypt skipped: no key ratchet");
		}
		return false;
	}

	if (!HasAssignedSsrc_ || AssignedSsrc_ != Ssrc) {
		Api_->Fn->EncryptorAssignSsrc(Encryptor_, Ssrc, 1);
		AssignedSsrc_ = Ssrc;
		HasAssignedSsrc_ = true;
	}

	const std::size_t Capacity = Api_->Fn->EncryptorMaxCiphertext(Encryptor_, 0, Opus.size());
	if (Capacity == 0) {
		return false;
	}

	Out.resize(Capacity);
	std::size_t Written = 0;
	const int Result = Api_->Fn->EncryptorEncrypt(
		Encryptor_,
		0,
		Ssrc,
		Opus.data(),
		Opus.size(),
		Out.data(),
		Out.size(),
		&Written
	);
	if (Result != 0) {
		if (OnDebug) {
			OnDebug("encrypt failed with code " + std::to_string(Result));
		}
		Out.clear();
		return false;
	}

	Out.resize(Written);
	if (OnDebug && !FrameLogged_) {
		FrameLogged_ = true;
		OnDebug(
			"frame encrypted " + std::to_string(Opus.size()) + " -> "
			+ std::to_string(Out.size()) + " bytes"
		);
	}
	return true;
}

void DaveSession::FailureCallback(const char* Source, const char* Reason, void* UserData) {
	auto* Self = static_cast<DaveSession*>(UserData);
	if (!Self || !Self->OnFailure) {
		return;
	}
	if (Reason && std::string(Reason).find("GetPersistedKeyPair") != std::string::npos) {
		return;
	}

	std::string Message = Source ? Source : "DAVE";
	if (Reason && *Reason) {
		Message += ": ";
		Message += Reason;
	}
	Self->OnFailure(Message);
}

void DaveSession::CaptureRatchet(std::uint16_t TransitionId) {
	void* Ratchet = Api_->Fn->SessionGetKeyRatchet(Session_, SelfUserId_.c_str());
	if (!Ratchet) {
		return;
	}

	if (TransitionId == 0) {
		ApplyRatchet(Ratchet);
	} else {
		StoreRatchet(TransitionId, Ratchet);
	}
}

void DaveSession::ApplyRatchet(void* Ratchet) {
	if (!Ratchet) {
		return;
	}
	if (!Api_) {
		return;
	}
	if (!Encryptor_) {
		Api_->Fn->KeyRatchetDestroy(Ratchet);
		return;
	}

	Api_->Fn->EncryptorSetKeyRatchet(Encryptor_, Ratchet);
	Api_->Fn->KeyRatchetDestroy(Ratchet);
	SetPassthrough(false);
	if (OnDebug) {
		OnDebug("key ratchet applied");
	}
}

void DaveSession::StoreRatchet(std::uint16_t TransitionId, void* Ratchet) {
	const auto Found = PendingRatchets_.find(TransitionId);
	if (Found != PendingRatchets_.end()) {
		Api_->Fn->KeyRatchetDestroy(Found->second);
		PendingRatchets_.erase(Found);
	}
	PendingRatchets_[TransitionId] = Ratchet;
}

void DaveSession::ApplyPending(std::uint16_t TransitionId) {
	const auto Found = PendingRatchets_.find(TransitionId);
	if (Found == PendingRatchets_.end()) {
		return;
	}

	void* Ratchet = Found->second;
	PendingRatchets_.erase(Found);
	ApplyRatchet(Ratchet);
}

void DaveSession::ClearPending() {
	if (!Api_) {
		return;
	}
	for (auto& Entry : PendingRatchets_) {
		Api_->Fn->KeyRatchetDestroy(Entry.second);
	}
	PendingRatchets_.clear();
	PendingVersions_.clear();
}

void DaveSession::SetPassthrough(bool Enabled) {
	if (Api_ && Encryptor_) {
		Api_->Fn->EncryptorSetPassthrough(Encryptor_, Enabled);
	}
}

}
