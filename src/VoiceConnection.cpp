#include "Voice/VoiceConnection.h"

#include <string>
#include <utility>

#include "Voice/AudioPlayer.h"

namespace Voice {

VoiceConnection::VoiceConnection(JoinConfig Config, ConnectionOptions Options, bool Debug)
	: Config_(std::move(Config)) {
	Network_ = std::make_unique<Networking>(std::move(Options), Debug);

	Network_->OnStateChange = [this](NetworkingStatusCode, NetworkingStatusCode Next) {
		if (Next == NetworkingStatusCode::Ready) {
			SetStatus(VoiceConnectionStatus::Ready);
		} else if (Next == NetworkingStatusCode::Closed) {
			SetStatus(VoiceConnectionStatus::Disconnected);
		}
	};
	Network_->OnReady = [this] {
		SetStatus(VoiceConnectionStatus::Ready);
		if (OnReady) {
			OnReady();
		}
	};
	Network_->OnClose = [this](int Code) {
		SetStatus(VoiceConnectionStatus::Disconnected);
		if (OnClose) {
			OnClose(Code);
		}
	};
	Network_->OnError = [this](const std::string& Message) {
		if (OnError) {
			OnError(Message);
		}
	};
	Network_->OnDebug = [this](const std::string& Message) {
		if (OnDebug) {
			OnDebug(Message);
		}
	};
}

VoiceConnection::~VoiceConnection() {
	if (Network_) {
		Network_->Destroy();
	}
}

void VoiceConnection::Start() {
	Status_ = VoiceConnectionStatus::Connecting;
	if (Network_) {
		Network_->Start();
	}
}

VoiceConnectionStatus VoiceConnection::Status() const {
	return Status_.load();
}

const JoinConfig& VoiceConnection::Config() const {
	return Config_;
}

Networking& VoiceConnection::Network() {
	return *Network_;
}

std::optional<OpusPacket> VoiceConnection::PrepareAudioPacket(
	const OpusPacket& Packet,
	std::uint16_t Sequence,
	std::uint32_t Timestamp
) {
	if (Status_.load() != VoiceConnectionStatus::Ready || !Network_) {
		return std::nullopt;
	}
	return Network_->PrepareAudioPacket(Packet, Sequence, Timestamp);
}

bool VoiceConnection::PrepareAudioPacketFast(
	const OpusPacket& Packet,
	std::uint16_t Sequence,
	std::uint32_t Timestamp
) {
	if (Status_.load() != VoiceConnectionStatus::Ready || !Network_) {
		return false;
	}
	return Network_->PrepareAudioPacketFast(Packet, Sequence, Timestamp);
}

bool VoiceConnection::DispatchAudio() {
	if (Status_.load() != VoiceConnectionStatus::Ready || !Network_) {
		return false;
	}
	return Network_->DispatchAudio();
}

std::optional<OpusPacket> VoiceConnection::PlayOpusPacket(const OpusPacket& Packet) {
	if (Status_.load() != VoiceConnectionStatus::Ready || !Network_) {
		return std::nullopt;
	}
	std::optional<OpusPacket> Prepared = Network_->PrepareAudioPacket(Packet);
	if (Prepared) {
		Network_->DispatchAudio();
	}
	return Prepared;
}

void VoiceConnection::SetSpeaking(bool Enabled) {
	if (Status_.load() != VoiceConnectionStatus::Ready || !Network_) {
		return;
	}
	Network_->SetSpeaking(Enabled);
}

bool VoiceConnection::DaveReady() const {
	return Status_.load() == VoiceConnectionStatus::Ready && Network_ && Network_->DaveReady();
}

void VoiceConnection::Subscribe(AudioPlayer* Player) {
	Player_ = Player;
	if (Player_) {
		Player_->Subscribe(this);
	}
}

void VoiceConnection::Disconnect() {
	if (Status_.load() == VoiceConnectionStatus::Destroyed) {
		return;
	}
	SetStatus(VoiceConnectionStatus::Disconnected);
	if (Network_) {
		Network_->Destroy();
	}
}

void VoiceConnection::Destroy() {
	if (Status_.load() == VoiceConnectionStatus::Destroyed) {
		return;
	}
	SetStatus(VoiceConnectionStatus::Destroyed);
	if (Network_) {
		Network_->Destroy();
	}
}

PingInfo VoiceConnection::Ping() const {
	if (Status_.load() == VoiceConnectionStatus::Ready && Network_) {
		return Network_->Ping();
	}
	return {};
}

void VoiceConnection::SetStatus(VoiceConnectionStatus Next) {
	const VoiceConnectionStatus Previous = Status_.exchange(Next);
	if (Previous == Next) {
		return;
	}
	if (OnStateChange) {
		OnStateChange(Previous, Next);
	}
}

}
