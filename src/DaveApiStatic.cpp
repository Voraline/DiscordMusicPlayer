#include "DaveApi.h"

#include <dave/dave.h>

namespace Voice::DaveApi {

std::shared_ptr<Functions> Load() {
	static const std::shared_ptr<Functions> Instance = [] {
		auto FunctionsPtr = std::make_shared<Functions>();

		FunctionsPtr->MaxProtocolVersion = &daveMaxSupportedProtocolVersion;
		FunctionsPtr->Free = &daveFree;
		FunctionsPtr->SessionCreate = reinterpret_cast<decltype(FunctionsPtr->SessionCreate)>(
			&daveSessionCreate
		);
		FunctionsPtr->SessionDestroy = reinterpret_cast<decltype(FunctionsPtr->SessionDestroy)>(
			&daveSessionDestroy
		);
		FunctionsPtr->SessionInit = reinterpret_cast<decltype(FunctionsPtr->SessionInit)>(
			&daveSessionInit
		);
		FunctionsPtr->SessionReset = reinterpret_cast<decltype(FunctionsPtr->SessionReset)>(
			&daveSessionReset
		);
		FunctionsPtr->SessionSetProtocolVersion = reinterpret_cast<
			decltype(FunctionsPtr->SessionSetProtocolVersion)
		>(&daveSessionSetProtocolVersion);
		FunctionsPtr->SessionSetExternalSender = reinterpret_cast<
			decltype(FunctionsPtr->SessionSetExternalSender)
		>(&daveSessionSetExternalSender);
		FunctionsPtr->SessionProcessProposals = reinterpret_cast<
			decltype(FunctionsPtr->SessionProcessProposals)
		>(&daveSessionProcessProposals);
		FunctionsPtr->SessionProcessCommit = reinterpret_cast<
			decltype(FunctionsPtr->SessionProcessCommit)
		>(&daveSessionProcessCommit);
		FunctionsPtr->SessionProcessWelcome = reinterpret_cast<
			decltype(FunctionsPtr->SessionProcessWelcome)
		>(&daveSessionProcessWelcome);
		FunctionsPtr->SessionGetKeyPackage = reinterpret_cast<
			decltype(FunctionsPtr->SessionGetKeyPackage)
		>(&daveSessionGetMarshalledKeyPackage);
		FunctionsPtr->SessionGetKeyRatchet = reinterpret_cast<
			decltype(FunctionsPtr->SessionGetKeyRatchet)
		>(&daveSessionGetKeyRatchet);
		FunctionsPtr->KeyRatchetDestroy = reinterpret_cast<decltype(FunctionsPtr->KeyRatchetDestroy)>(
			&daveKeyRatchetDestroy
		);
		FunctionsPtr->CommitIsFailed = reinterpret_cast<decltype(FunctionsPtr->CommitIsFailed)>(
			&daveCommitResultIsFailed
		);
		FunctionsPtr->CommitIsIgnored = reinterpret_cast<decltype(FunctionsPtr->CommitIsIgnored)>(
			&daveCommitResultIsIgnored
		);
		FunctionsPtr->CommitDestroy = reinterpret_cast<decltype(FunctionsPtr->CommitDestroy)>(
			&daveCommitResultDestroy
		);
		FunctionsPtr->WelcomeDestroy = reinterpret_cast<decltype(FunctionsPtr->WelcomeDestroy)>(
			&daveWelcomeResultDestroy
		);
		FunctionsPtr->EncryptorCreate = reinterpret_cast<decltype(FunctionsPtr->EncryptorCreate)>(
			&daveEncryptorCreate
		);
		FunctionsPtr->EncryptorDestroy = reinterpret_cast<decltype(FunctionsPtr->EncryptorDestroy)>(
			&daveEncryptorDestroy
		);
		FunctionsPtr->EncryptorSetKeyRatchet = reinterpret_cast<
			decltype(FunctionsPtr->EncryptorSetKeyRatchet)
		>(&daveEncryptorSetKeyRatchet);
		FunctionsPtr->EncryptorSetPassthrough = reinterpret_cast<
			decltype(FunctionsPtr->EncryptorSetPassthrough)
		>(&daveEncryptorSetPassthroughMode);
		FunctionsPtr->EncryptorAssignSsrc = reinterpret_cast<
			decltype(FunctionsPtr->EncryptorAssignSsrc)
		>(&daveEncryptorAssignSsrcToCodec);
		FunctionsPtr->EncryptorMaxCiphertext = reinterpret_cast<
			decltype(FunctionsPtr->EncryptorMaxCiphertext)
		>(&daveEncryptorGetMaxCiphertextByteSize);
		FunctionsPtr->EncryptorEncrypt = reinterpret_cast<decltype(FunctionsPtr->EncryptorEncrypt)>(
			&daveEncryptorEncrypt
		);
		FunctionsPtr->EncryptorHasKeyRatchet = reinterpret_cast<
			decltype(FunctionsPtr->EncryptorHasKeyRatchet)
		>(&daveEncryptorHasKeyRatchet);
		FunctionsPtr->EncryptorIsPassthroughMode = reinterpret_cast<
			decltype(FunctionsPtr->EncryptorIsPassthroughMode)
		>(&daveEncryptorIsPassthroughMode);
		FunctionsPtr->EncryptorGetProtocolVersion = reinterpret_cast<
			decltype(FunctionsPtr->EncryptorGetProtocolVersion)
		>(&daveEncryptorGetProtocolVersion);
		FunctionsPtr->SetLogSink = reinterpret_cast<decltype(FunctionsPtr->SetLogSink)>(
			&daveSetLogSinkCallback
		);

		daveSetLogSinkCallback([](DAVELoggingSeverity, const char*, int, const char*) {});

		return FunctionsPtr;
	}();
	return Instance;
}

}
