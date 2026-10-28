#include "Voice/VoiceClient.h"

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Platform.h"
#include "Voice/AudioPlayer.h"
#include "Voice/AudioResource.h"
#include "Voice/GatewayClient.h"
#include "Voice/OpusCodec.h"
#include "Voice/Secretbox.h"
#include "Voice/VoiceConnection.h"

namespace Voice {

namespace {

constexpr std::uint32_t TimestampIncrement = 960;
constexpr int FrameMilliseconds = 20;
constexpr int MaxRejoinAttempts = 6;
constexpr auto HandshakeTimeout = std::chrono::milliseconds(6000);

std::int64_t SteadyNowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::steady_clock::now().time_since_epoch()
	)
		.count();
}

std::uint16_t Random16() {
	std::uint8_t Bytes[2]{};
	Secretbox::RandomBytes(Bytes);
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(Bytes[0]) << 8) | Bytes[1]);
}

std::uint32_t Random32() {
	std::uint8_t Bytes[4]{};
	Secretbox::RandomBytes(Bytes);
	return (static_cast<std::uint32_t>(Bytes[0]) << 24)
		| (static_cast<std::uint32_t>(Bytes[1]) << 16)
		| (static_cast<std::uint32_t>(Bytes[2]) << 8)
		| static_cast<std::uint32_t>(Bytes[3]);
}

void PrecisionWaitUntil(
	std::chrono::steady_clock::time_point Deadline,
	const std::atomic<bool>& Stopping
) {
	constexpr auto SpinWindow = std::chrono::milliseconds(1);
	const auto Now = std::chrono::steady_clock::now();
	if (Deadline > Now + SpinWindow) {
		std::this_thread::sleep_until(Deadline - SpinWindow);
	}
	while (!Stopping.load(std::memory_order_relaxed)) {
		if (std::chrono::steady_clock::now() >= Deadline) {
			break;
		}
		Platform::CpuPause();
	}
}

}

struct VoiceClient::Impl {
	struct Bot {
		BotOptions Options;
		std::unique_ptr<GatewayClient> Gateway;
		std::shared_ptr<VoiceConnection> Connection;
		std::shared_ptr<AudioPlayer> Player;
		std::shared_ptr<AudioResource> Resource;
		std::atomic<bool> Ready{false};
		std::atomic<bool> Failed{false};
		std::atomic<std::uint64_t> Generation{0};
		std::atomic<std::int64_t> ConnectStarted{0};
		std::atomic<int> RejoinAttempts{0};
	};

	struct Playback {
		Bot* Entry;
		std::shared_ptr<VoiceConnection> Connection;
		std::shared_ptr<AudioPlayer> Player;
	};

	VoiceClient* Owner;
	JoinConfig Config;
	PlaybackOptions Settings;
	std::vector<std::unique_ptr<Bot>> Bots;

	mutable std::mutex Mutex;
	std::condition_variable Condition;
	std::size_t ReadyCount = 0;
	std::size_t FailedCount = 0;
	bool Finished = true;
	std::atomic<bool> Stopping{false};
	std::atomic<bool> Playing{false};
	std::string Error;

	std::thread Scheduler;
	std::jthread Connector;
	std::jthread Supervisor;

	Impl(
		VoiceClient* OwnerPointer,
		std::vector<BotOptions> InputBots,
		JoinConfig ConfigValue,
		PlaybackOptions PlaybackSettings
	)
		: Owner(OwnerPointer),
		  Config(std::move(ConfigValue)),
		  Settings(std::move(PlaybackSettings)) {
#if defined(_WIN32)
		timeBeginPeriod(1);
#endif

		for (BotOptions& Input : InputBots) {
			auto BotPointer = std::make_unique<Bot>();
			BotPointer->Options = std::move(Input);
			if (BotPointer->Options.Name.empty()) {
				BotPointer->Options.Name = "Bot" + std::to_string(Bots.size() + 1);
			}

			Bot* Raw = BotPointer.get();
			Raw->Gateway = std::make_unique<GatewayClient>(
				Raw->Options.Token,
				Raw->Options.SelfBot,
				Settings.Debug
			);
			Raw->Gateway->OnReady = [this, Raw] { Raw->Gateway->Join(Config); };
			Raw->Gateway->OnVoiceReady = [this, Raw](const VoiceServerInfo& Server, const VoiceStateInfo& State) {
				CreateConnection(Raw, Server, State);
			};
			Raw->Gateway->OnError = [this, Raw](const std::string& Message) {
				Report(Raw->Options.Name, Message);
			};
			Raw->Gateway->OnDebug = [this, Raw](const std::string& Message) {
				Debug("[" + Raw->Options.Name + "] " + Message);
			};
			Raw->Gateway->OnClose = [this, Raw](int Code) {
				if (Stopping.load()) {
					return;
				}
				if (Raw->Failed.exchange(true)) {
					return;
				}
				Report(
					Raw->Options.Name,
					Code != 0 && Code != 1000
						? "Gateway closed with code " + std::to_string(Code)
						: "Gateway connection closed"
				);
				const bool WasReady = Raw->Ready.exchange(false);
				{
					std::lock_guard Lock(Mutex);
					if (WasReady && ReadyCount > 0) {
						--ReadyCount;
					}
					++FailedCount;
				}
				Condition.notify_all();
			};

			Bots.push_back(std::move(BotPointer));
		}
	}

	~Impl() {
		Disconnect();
		Bots.clear();
#if defined(_WIN32)
		timeEndPeriod(1);
#endif
	}

	void Report(const std::string& BotName, const std::string& Message) {
		{
			std::lock_guard Lock(Mutex);
			Error = BotName + ": " + Message;
		}
		if (Owner->OnError) {
			Owner->OnError(BotName, Message);
		}
	}

	void Debug(const std::string& Message) {
		if (!Settings.Debug) {
			return;
		}
		if (Owner->OnDebug) {
			Owner->OnDebug(Message);
		}
	}

	void CreateConnection(Bot* Target, const VoiceServerInfo& Server, const VoiceStateInfo& State) {
		if (Playing.load() || Target->Ready.load()) {
			Debug("[" + Target->Options.Name + "] ignoring redundant voice session update");
			return;
		}

		ConnectionOptions Options;
		Options.ServerId = Config.GuildId;
		Options.ChannelId = Config.ChannelId;
		Options.UserId = Target->Gateway->UserId();
		Options.SessionId = State.SessionId;
		Options.Token = Server.Token;
		Options.Endpoint = Server.Endpoint;

		const std::uint64_t Generation = Target->Generation.fetch_add(1) + 1;

		std::shared_ptr<AudioPlayer> PreviousPlayer = std::move(Target->Player);
		std::shared_ptr<VoiceConnection> PreviousConnection = std::move(Target->Connection);
		if (PreviousPlayer) {
			PreviousPlayer->ClearSubscribers();
		}
		if (PreviousConnection) {
			PreviousConnection->Destroy();
		}

		const bool WasReady = Target->Ready.exchange(false);
		Target->ConnectStarted = SteadyNowMs();
		if (WasReady) {
			std::lock_guard Lock(Mutex);
			if (ReadyCount > 0) {
				--ReadyCount;
			}
		}

		Target->Player = std::make_unique<AudioPlayer>();
		Target->Player->SetNoSubscriberBehavior(NoSubscriberBehavior::Pause);
		Target->Player->OnDebug = [this, Target](const std::string& Message) {
			Debug("[" + Target->Options.Name + "] " + Message);
		};

		Target->Connection = std::make_unique<VoiceConnection>(Config, Options, Settings.Debug);
		Target->Connection->Subscribe(Target->Player.get());
		Target->Connection->OnReady = [this, Target, Generation] {
			if (Target->Generation.load() != Generation) {
				return;
			}
			HandleConnectionReady(Target);
		};
		Target->Connection->OnClose = [this, Target, Generation](int Code) {
			if (Target->Generation.load() != Generation) {
				return;
			}
			HandleConnectionClosed(Target, Code);
		};
		Target->Connection->OnError = [this, Target, Generation](const std::string& Message) {
			if (Target->Generation.load() != Generation) {
				return;
			}
			Report(Target->Options.Name, Message);
		};
		Target->Connection->OnDebug = [this, Target](const std::string& Message) {
			Debug("[" + Target->Options.Name + "] " + Message);
		};
		Target->Connection->Start();
	}

	void HandleConnectionReady(Bot* Target) {
		bool Increment = false;
		{
			std::lock_guard Lock(Mutex);
			if (!Target->Ready.exchange(true)) {
				++ReadyCount;
				Increment = true;
			}
		}
		Target->RejoinAttempts = 0;
		Target->ConnectStarted = 0;
		Target->Failed = false;
		if (Increment) {
			Debug("[" + Target->Options.Name + "] voice ready");
		}
		Condition.notify_all();
	}

	void HandleConnectionClosed(Bot* Target, int Code) {
		const bool WasReady = Target->Ready.exchange(false);
		if (WasReady) {
			std::lock_guard Lock(Mutex);
			if (ReadyCount > 0) {
				--ReadyCount;
			}
		}
		Target->ConnectStarted = SteadyNowMs();
		if (WasReady && Code == 4006) {
			Report(
				Target->Options.Name,
				"Voice session invalidated (4006); another client/session likely took over this account"
			);
		} else if (WasReady && Code != 0 && Code != 1000) {
			Report(Target->Options.Name, "Voice connection closed with code " + std::to_string(Code));
		} else {
			Debug(
				"[" + Target->Options.Name + "] voice connection closed (code "
				+ std::to_string(Code) + "), retrying"
			);
		}
		Condition.notify_all();
	}

	void RunSupervisor(std::stop_token Token) {
		while (!Token.stop_requested()) {
			for (int Step = 0; Step < 5 && !Token.stop_requested(); ++Step) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (Token.stop_requested()) {
				break;
			}
			if (Stopping.load() || Playing.load()) {
				continue;
			}

			const std::int64_t Now = SteadyNowMs();
			for (std::unique_ptr<Bot>& Entry : Bots) {
				Bot* Target = Entry.get();
				if (Target->Failed.load()) {
					continue;
				}
				if (Target->Ready.load()) {
					Target->RejoinAttempts = 0;
					Target->ConnectStarted = 0;
					continue;
				}
				if (!Target->Gateway->IsReady()) {
					continue;
				}

				const std::int64_t Started = Target->ConnectStarted.load();
				if (Started == 0) {
					Target->ConnectStarted = Now;
					continue;
				}
				if (Now - Started < HandshakeTimeout.count()) {
					continue;
				}

				const int Attempt = Target->RejoinAttempts.fetch_add(1) + 1;
				if (Attempt > MaxRejoinAttempts) {
					Target->Failed = true;
					Report(
						Target->Options.Name,
						"Voice handshake did not complete after "
							+ std::to_string(MaxRejoinAttempts) + " rejoin attempts"
					);
					{
						std::lock_guard Lock(Mutex);
						++FailedCount;
					}
					Condition.notify_all();
					continue;
				}

				Debug(
					"[" + Target->Options.Name + "] voice handshake timeout, rejoining ("
					+ std::to_string(Attempt) + "/" + std::to_string(MaxRejoinAttempts) + ")"
				);
				Target->ConnectStarted = Now;
				Target->Gateway->Join(Config);
			}
		}
	}

	bool Start() {
		if (Bots.empty()) {
			Report("client", "No bots configured");
			return false;
		}

		Supervisor.request_stop();
		if (Supervisor.joinable()) {
			Supervisor.join();
		}
		Connector.request_stop();
		if (Connector.joinable()) {
			Connector.join();
		}

		{
			std::lock_guard Lock(Mutex);
			ReadyCount = 0;
			FailedCount = 0;
			Error.clear();
		}
		for (std::unique_ptr<Bot>& Entry : Bots) {
			Entry->Generation.fetch_add(1);
			Entry->Ready = false;
			Entry->Failed = false;
			Entry->ConnectStarted = 0;
			Entry->RejoinAttempts = 0;
		}

		Supervisor = std::jthread([this](std::stop_token Token) { RunSupervisor(Token); });

		Connector = std::jthread([this](std::stop_token Token) {
			for (std::size_t Index = 0; Index < Bots.size(); ++Index) {
				if (Token.stop_requested()) {
					return;
				}
				Bots[Index]->Gateway->Connect();

				if (Index + 1 < Bots.size()) {
					const auto Deadline = std::chrono::steady_clock::now() + Settings.ConnectStagger;
					while (std::chrono::steady_clock::now() < Deadline && !Token.stop_requested()) {
						std::this_thread::sleep_for(std::chrono::milliseconds(25));
					}
				}
			}
		});

		std::unique_lock Lock(Mutex);
		Condition.wait_for(Lock, Settings.ConnectTimeout, [this] {
			return ReadyCount >= Bots.size() || ReadyCount + FailedCount >= Bots.size();
		});

		if (ReadyCount < Bots.size() && Error.empty()) {
			const Bot* First = Bots.front().get();
			const bool GatewayReady = First->Gateway && First->Gateway->IsReady();
			const bool VoicePackets = First->Gateway && First->Gateway->IsVoiceReady();
			if (!GatewayReady) {
				Error = "Timed out waiting for gateway READY (invalid token or selfbot mode mismatch)";
			} else if (!VoicePackets) {
				Error = "Gateway READY, but no voice server/state packets (check guild id, channel id, permissions)";
			} else {
				Error = "Voice gateway/UDP handshake did not complete";
			}
		}
		return ReadyCount >= Bots.size();
	}

	bool WaitForReady(std::chrono::milliseconds Timeout) {
		std::unique_lock Lock(Mutex);
		Condition.wait_for(Lock, Timeout, [this] {
			return ReadyCount >= Bots.size() || ReadyCount + FailedCount >= Bots.size();
		});
		return ReadyCount >= Bots.size();
	}

	void PlayFile(const std::filesystem::path& File) {
		EncoderOptions Encoder;
		Encoder.Bitrate = Settings.Bitrate;
		Encoder.Complexity = Settings.OpusComplexity;
		Encoder.FfmpegPath = Settings.FfmpegPath;
		Encoder.Debug = Settings.Debug;

		std::shared_ptr<EncodedAudio> Audio = OpusCodec::EncodeFile(File, Encoder);
		Play(std::move(Audio));
	}

	void Play(std::shared_ptr<EncodedAudio> Audio) {
		if (!Audio || Audio->Frames.empty()) {
			throw std::runtime_error("Audio resource is empty");
		}

		Stop();

		std::vector<Playback> Active;
		{
			std::lock_guard Lock(Mutex);
			for (std::unique_ptr<Bot>& Entry : Bots) {
				if (Entry->Connection && Entry->Player
					&& Entry->Connection->Status() == VoiceConnectionStatus::Ready) {
					Active.push_back(Playback{Entry.get(), Entry->Connection, Entry->Player});
				}
			}
			if (Active.empty()) {
				throw std::runtime_error("No ready voice connections");
			}
			Finished = false;
			Stopping = false;
			Playing = true;
		}

		const auto DaveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (const Playback& Target : Active) {
			bool Warned = false;
			while (!Target.Connection->DaveReady()) {
				if (!Warned) {
					Debug("[" + Target.Entry->Options.Name + "] waiting for DAVE");
					Warned = true;
				}
				if (Stopping.load() || std::chrono::steady_clock::now() >= DaveDeadline) {
					Playing = false;
					throw std::runtime_error(
						"DAVE encryption is not ready for " + Target.Entry->Options.Name
					);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		for (const Playback& Target : Active) {
			Target.Entry->Resource = std::make_shared<AudioResource>(Audio, Settings.SilencePaddingFrames);
			Target.Player->Play(Target.Entry->Resource);
			Target.Connection->SetSpeaking(true);
		}

		Scheduler = std::thread([this, Audio, Active] { RunScheduler(Audio, Active); });
	}

	void RunScheduler(const std::shared_ptr<EncodedAudio>& Audio, const std::vector<Playback>& Active) {
		bool MmcssOk = false;
		bool PriorityOk = false;
#if defined(_WIN32)
		DWORD MmcssTask = 0;
		HANDLE Mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &MmcssTask);
		MmcssOk = Mmcss != nullptr;
		PriorityOk = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
		sched_param PriorityParam{};
		PriorityParam.sched_priority = 80;
		PriorityOk = pthread_setschedparam(pthread_self(), SCHED_FIFO, &PriorityParam) == 0;
#endif

		if (Settings.Debug) {
			Debug(
				"[SYNC] mmcss=" + std::string(MmcssOk ? "1" : "0")
				+ " timeCritical=" + std::string(PriorityOk ? "1" : "0")
			);
		}

		const auto StartTime = std::chrono::steady_clock::now() + Settings.LeadTime;
		const std::uint16_t BaseSequence = Random16();
		const std::uint32_t BaseTimestamp = Random32();

		for (const Playback& Target : Active) {
			Target.Player->StepPrepare(BaseSequence, BaseTimestamp);
		}

		const bool MeasureSync = Settings.Debug;
		std::array<long long, 200> SyncSamples{};
		std::size_t SyncCount = 0;
		long long MaxSkewNs = 0;
		bool SyncReported = false;

		auto ReportSync = [&] {
			if (!MeasureSync || SyncCount == 0 || SyncReported) {
				return;
			}
			SyncReported = true;
			std::sort(SyncSamples.begin(), SyncSamples.begin() + SyncCount);
			const long long P50 = SyncSamples[SyncCount * 50 / 100];
			const long long P95 = SyncSamples[std::min(SyncCount - 1, SyncCount * 95 / 100)];
			Debug(
				"[SYNC] local dispatch p50=" + std::to_string(static_cast<double>(P50) / 1000.0)
				+ "us p95=" + std::to_string(static_cast<double>(P95) / 1000.0)
				+ "us max=" + std::to_string(static_cast<double>(MaxSkewNs) / 1000.0) + "us"
			);
		};

		const std::size_t MaximumFrames = Audio->Frames.size()
			+ static_cast<std::size_t>(Settings.SilencePaddingFrames) + 100;

		for (std::size_t Frame = 0; Frame < MaximumFrames; ++Frame) {
			bool AnyActive = false;
			for (const Playback& Target : Active) {
				if (Target.Player->Status() != AudioPlayerStatus::Idle) {
					AnyActive = true;
					break;
				}
			}
			if (!AnyActive || Stopping.load()) {
				break;
			}

			PrecisionWaitUntil(
				StartTime + std::chrono::milliseconds(static_cast<std::int64_t>(Frame) * FrameMilliseconds),
				Stopping
			);
			if (Stopping.load()) {
				break;
			}

			if (MeasureSync && Frame >= 20 && Frame < 220 && Active.size() > 1) {
				const auto First = std::chrono::steady_clock::now();
				auto Last = First;
				for (std::size_t Index = 0; Index < Active.size(); ++Index) {
					Last = std::chrono::steady_clock::now();
					Active[Index].Connection->DispatchAudio();
				}
				const long long SkewNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
					Last - First
				)
					.count();
				MaxSkewNs = std::max(MaxSkewNs, SkewNs);
				if (SyncCount < SyncSamples.size()) {
					SyncSamples[SyncCount++] = SkewNs;
				}
				if (SyncCount == SyncSamples.size()) {
					ReportSync();
				}
			} else {
				for (const Playback& Target : Active) {
					Target.Connection->DispatchAudio();
				}
			}

			const std::uint16_t Sequence = static_cast<std::uint16_t>(BaseSequence + Frame + 1);
			const std::uint32_t Timestamp = BaseTimestamp
				+ static_cast<std::uint32_t>((Frame + 1) * TimestampIncrement);

			for (const Playback& Target : Active) {
				Target.Player->StepPrepare(Sequence, Timestamp);
			}
		}

		ReportSync();

		Playing = false;
		for (const Playback& Target : Active) {
			Target.Connection->SetSpeaking(false);
			Target.Player->Stop(true);
		}

#if defined(_WIN32)
		if (Mmcss) {
			AvRevertMmThreadCharacteristics(Mmcss);
		}
#endif

		{
			std::lock_guard Lock(Mutex);
			Finished = true;
			Stopping = false;
		}
		Condition.notify_all();

		if (Owner->OnFinished) {
			Owner->OnFinished();
		}
	}

	void Stop() {
		Playing = false;
		Stopping = true;
		Condition.notify_all();

		if (Scheduler.joinable() && Scheduler.get_id() != std::this_thread::get_id()) {
			Scheduler.join();
		}

		{
			std::lock_guard Lock(Mutex);
			for (std::unique_ptr<Bot>& Entry : Bots) {
				if (Entry->Player) {
					Entry->Player->Stop(true);
				}
				if (Entry->Connection && Entry->Connection->Status() == VoiceConnectionStatus::Ready) {
					Entry->Connection->SetSpeaking(false);
				}
			}
			Finished = true;
			Stopping = false;
		}
		Condition.notify_all();
	}

	void Disconnect() {
		Stopping = true;
		Supervisor.request_stop();
		if (Supervisor.joinable() && Supervisor.get_id() != std::this_thread::get_id()) {
			Supervisor.join();
		}

		Stop();

		Connector.request_stop();
		if (Connector.joinable() && Connector.get_id() != std::this_thread::get_id()) {
			Connector.join();
		}

		for (std::unique_ptr<Bot>& Entry : Bots) {
			Entry->Generation.fetch_add(1);
			if (Entry->Connection) {
				Entry->Connection->Destroy();
				Entry->Connection.reset();
			}
			if (Entry->Player) {
				Entry->Player->ClearSubscribers();
				Entry->Player.reset();
			}
			if (Entry->Gateway) {
				Entry->Gateway->Leave();
				Entry->Gateway->Close();
			}
			Entry->Ready = false;
		}

		{
			std::lock_guard Lock(Mutex);
			ReadyCount = 0;
			FailedCount = 0;
		}
		Condition.notify_all();
		Stopping = false;
	}
};

VoiceClient::VoiceClient(std::vector<BotOptions> Bots, JoinConfig Config, PlaybackOptions Options)
	: Impl_(std::make_unique<Impl>(this, std::move(Bots), std::move(Config), std::move(Options))) {}

VoiceClient::~VoiceClient() {
	Impl_.reset();
}

bool VoiceClient::Start() {
	return Impl_->Start();
}

void VoiceClient::PlayFile(const std::filesystem::path& File) {
	Impl_->PlayFile(File);
}

void VoiceClient::Play(std::shared_ptr<EncodedAudio> Audio) {
	Impl_->Play(std::move(Audio));
}

void VoiceClient::Stop() {
	Impl_->Stop();
}

void VoiceClient::Disconnect() {
	Impl_->Disconnect();
}

bool VoiceClient::WaitForFinished() {
	std::unique_lock Lock(Impl_->Mutex);
	Impl_->Condition.wait(Lock, [this] { return Impl_->Finished; });
	return true;
}

bool VoiceClient::WaitForReady(std::chrono::milliseconds Timeout) {
	return Impl_->WaitForReady(Timeout);
}

std::size_t VoiceClient::ReadyCount() const {
	std::lock_guard Lock(Impl_->Mutex);
	return Impl_->ReadyCount;
}

std::size_t VoiceClient::BotCount() const {
	return Impl_->Bots.size();
}

std::string VoiceClient::LastError() const {
	std::lock_guard Lock(Impl_->Mutex);
	return Impl_->Error;
}

}
