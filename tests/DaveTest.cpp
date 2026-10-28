#include <iostream>
#include <string>

#include "DaveSession.h"

int main() {
	Voice::DaveSession Session;
	std::cout << "available=" << Session.Available()
			  << " maxVersion=" << Session.MaxProtocolVersion() << "\n";

	if (!Session.Available()) {
		return 1;
	}

	Session.OnFailure = [](const std::string& Message) {
		std::cerr << "dave failure: " << Message << "\n";
	};

	const bool Initialized = Session.Init(
		1,
		1238452317838377035ULL,
		"1359618198877634681",
		"test-session"
	);
	std::cout << "init=" << Initialized
			  << " version=" << Session.ProtocolVersion() << "\n";

	const auto KeyPackage = Session.GetKeyPackage();
	std::cout << "keyPackage=" << (KeyPackage ? KeyPackage->size() : 0) << "\n";

	return Initialized && KeyPackage && !KeyPackage->empty() ? 0 : 1;
}
