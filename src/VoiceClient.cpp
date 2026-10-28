#include "Voice/VoiceClient.h"

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#endif

#include <cstdlib>

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
		std::unique_ptr<VoiceConnection> Connection;
		std::unique_ptr<AudioPlayer> Player;
		std::shared_ptr<AudioResource> Resource;
		std::atomic<bool> Ready{false};
		std::atomic<bool> Failed{false};
		std::atomic<int> RejoinAttempts{0};
		std::jthread Rejoin;
		std::jthread Watchdog;
	};

	static constexpr int MaxRejoinAttempts = 5;
	static constexpr int HandshakeTimeoutSteps = 60;

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
				if (Code != 0 && Code != 1000) {
					Report(Raw->Options.Name, "Gateway closed with code " + std::to_string(Code));
				}
				{
					std::lock_guard Lock(Mutex);
					++FailedCount;
				}
				Condition.notify_all();
			};

			Bots.push_back(std::move(BotPointer));
		}
	}

	~Impl() {
		Disconnect();
		Stopping = true;
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
		auto StopThread = [](std::jthread& Thread) {
			if (!Thread.joinable()) {
				return;
			}
			Thread.request_stop();
			if (Thread.get_id() == std::this_thread::get_id()) {
				Thread.detach();
				return;
			}
			Thread.join();
		};
		StopThread(Target->Watchdog);
		StopThread(Target->Rejoin);

		ConnectionOptions Options;
		Options.ServerId = Config.GuildId;
		Options.ChannelId = Config.ChannelId;
		Options.UserId = Target->Gateway->UserId();
		Options.SessionId = State.SessionId;
		Options.Token = Server.Token;
		Options.Endpoint = Server.Endpoint;

		if (Target->Player) {
			Target->Player->ClearSubscribers();
			Target->Player.reset();
		}
		if (Target->Connection) {
			Target->Connection->Destroy();
			Target->Connection.reset();
		}

		Target->Ready = false;
		Target->Failed = false;
		Target->Player = std::make_unique<AudioPlayer>();
		Target->Player->SetNoSubscriberBehavior(NoSubscriberBehavior::Pause);
		Target->Player->OnDebug = [this, Target](const std::string& Message) {
			Debug("[" + Target->Options.Name + "] " + Message);
		};

		Target->Connection = std::make_unique<VoiceConnection>(Config, Options, Settings.Debug);
		Target->Connection->Subscribe(Target->Player.get());
		Target->Connection->OnReady = [this, Target] { HandleConnectionReady(Target); };
		Target->Connection->OnClose = [this, Target](int Code) { HandleConnectionClosed(Target, Code); };
		Target->Connection->OnError = [this, Target](const std::string& Message) {
			Report(Target->Options.Name, Message);
		};
		Target->Connection->OnDebug = [this, Target](const std::string& Message) {
			Debug("[" + Target->Options.Name + "] " + Message);
		};
		Target->Connection->Start();
		Target->Watchdog = std::jthread([this, Target](std::stop_token Token) {
			for (int Step = 0; Step < HandshakeTimeoutSteps && !Token.stop_requested(); ++Step) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (Token.stop_requested() || Stopping.load() || Playing.load()) {
				return;
			}
			if (Target->Ready.load()) {
				return;
			}
			Debug("[" + Target->Options.Name + "] voice handshake timeout");
			if (Target->Connection) {
				Target->Connection->Destroy();
			}
			ScheduleRejoin(Target);
		});
	}

	void ScheduleRejoin(Bot* Target) {
		if (Stopping.load() || Playing.load()) {
			return;
		}

		if (Target->Watchdog.joinable()) {
			Target->Watchdog.request_stop();
		}
		if (Target->Rejoin.joinable()) {
			Target->Rejoin.request_stop();
			if (Target->Rejoin.get_id() == std::this_thread::get_id()) {
				Target->Rejoin.detach();
			} else {
				Target->Rejoin.join();
			}
		}

		const int Attempt = Target->RejoinAttempts.fetch_add(1);
		if (Attempt >= MaxRejoinAttempts) {
			bool Expected = false;
			if (Target->Failed.compare_exchange_strong(Expected, true)) {
				{
					std::lock_guard Lock(Mutex);
					++FailedCount;
				}
				Report(Target->Options.Name, "Voice connection failed after retries");
				Condition.notify_all();
			}
			return;
		}

		const int DelayMs = 1000 + Attempt * 1000;
		Debug("[" + Target->Options.Name + "] retrying voice connection " + std::to_string(Attempt + 1));
		Target->Rejoin = std::jthread([this, Target, DelayMs](std::stop_token Token) {
			const int Steps = DelayMs / 100;
			for (int Step = 0; Step < Steps && !Token.stop_requested(); ++Step) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (Token.stop_requested() || Stopping.load() || Playing.load()) {
				return;
			}
			if (Target->Gateway) {
				Target->Gateway->Join(Config);
			}
		});
	}

	void HandleConnectionReady(Bot* Target) {
		Target->RejoinAttempts = 0;
		Target->Failed = false;
		if (Target->Watchdog.joinable()) {
			Target->Watchdog.request_stop();
		}
		if (Target->Rejoin.joinable()) {
			Target->Rejoin.request_stop();
		}

		bool Increment = false;
		{
			std::lock_guard Lock(Mutex);
			if (!Target->Ready.exchange(true)) {
				++ReadyCount;
				Increment = true;
			}
		}
		if (Increment) {
			Debug("[" + Target->Options.Name + "] voice ready");
		}
		Condition.notify_all();
	}

	void HandleConnectionClosed(Bot* Target, int Code) {
		const bool WasReady = Target->Ready.exchange(false);
		{
			std::lock_guard Lock(Mutex);
			if (WasReady && ReadyCount > 0) {
				--ReadyCount;
			}
		}

		if (Target->Watchdog.joinable()) {
			Target->Watchdog.request_stop();
		}

		const bool CanRetry = !Playing.load() && !Stopping.load();
		if (CanRetry) {
			Debug(
				"[" + Target->Options.Name + "] voice closed during startup with code "
				+ std::to_string(Code)
			);
			ScheduleRejoin(Target);
		} else if (Code == 4006) {
			Report(
				Target->Options.Name,
				"Voice session invalidated (4006); another client/session likely took over this account"
			);
		} else if (Code != 0 && Code != 1000) {
			Report(Target->Options.Name, "Voice connection closed with code " + std::to_string(Code));
		} else {
			Debug("[" + Target->Options.Name + "] voice connection closed");
		}
		Condition.notify_all();
	}

	bool Start() {
		if (Bots.empty()) {
			Report("client", "No bots configured");
			return false;
		}

		Connector.request_stop();
		if (Connector.joinable()) {
			Connector.join();
		}

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

		std::vector<Bot*> Active;
		{
			std::lock_guard Lock(Mutex);
			for (std::unique_ptr<Bot>& Entry : Bots) {
				if (Entry->Connection && Entry->Player
					&& Entry->Connection->Status() == VoiceConnectionStatus::Ready) {
					Active.push_back(Entry.get());
				}
			}
			if (Active.empty()) {
				throw std::runtime_error("No ready voice connections");
			}
			Finished = false;
			Stopping = false;
		}

		const auto DaveDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (Bot* Target : Active) {
			bool Warned = false;
			while (!Target->Connection->DaveReady()) {
				if (!Warned) {
					Debug("[" + Target->Options.Name + "] waiting for DAVE");
					Warned = true;
				}
				if (Stopping.load() || std::chrono::steady_clock::now() >= DaveDeadline) {
					throw std::runtime_error(
						"DAVE encryption is not ready for " + Target->Options.Name
					);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		Playing = true;
		for (Bot* Target : Active) {
			Target->Resource = std::make_shared<AudioResource>(Audio, Settings.SilencePaddingFrames);
			Target->Player->Play(Target->Resource);
			Target->Connection->SetSpeaking(true);
		}

		Scheduler = std::thread([this, Audio, Active] { RunScheduler(Audio, Active); });
	}

	void RunScheduler(const std::shared_ptr<EncodedAudio>& Audio, const std::vector<Bot*>& Active) {
		bool MmcssOk = false;
		bool PriorityOk = false;
#if defined(_WIN32)
		DWORD MmcssTask = 0;
		HANDLE Mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &MmcssTask);
		MmcssOk = Mmcss != nullptr;
		PriorityOk = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#else
		const bool Locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
		prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0);

		bool AffinityOk = false;
		int AffinityCpu = -1;
		const char* CpuValue = std::getenv("VOICE_CPU");
		if (CpuValue) {
			const int Cpu = std::atoi(CpuValue);
			if (Cpu >= 0) {
				cpu_set_t Set;
				CPU_ZERO(&Set);
				CPU_SET(Cpu, &Set);
				AffinityOk = sched_setaffinity(0, sizeof(Set), &Set) == 0;
				AffinityCpu = Cpu;
			}
		}

		sched_param PriorityParam{};
		const int MaxPriority = sched_get_priority_max(SCHED_FIFO);
		PriorityParam.sched_priority = MaxPriority > 0 ? MaxPriority - 1 : 1;
		PriorityOk = pthread_setschedparam(pthread_self(), SCHED_FIFO, &PriorityParam) == 0;
		if (!PriorityOk) {
			sched_param CurrentParam{};
			int CurrentPolicy = 0;
			if (pthread_getschedparam(pthread_self(), &CurrentPolicy, &CurrentParam) == 0
				&& CurrentPolicy == SCHED_FIFO) {
				PriorityOk = true;
			}
		}
#endif

		if (Settings.Debug) {
			std::string Info = "[SYNC] mmcss=" + std::string(MmcssOk ? "1" : "0")
				+ " timeCritical=" + std::string(PriorityOk ? "1" : "0");
#if !defined(_WIN32)
			Info += " mlock=" + std::string(Locked ? "1" : "0")
				+ " affinity=" + std::string(AffinityOk ? "1" : "0")
				+ " cpu=" + std::to_string(AffinityCpu);
#endif
			Debug(Info);
		}

		const auto StartTime = std::chrono::steady_clock::now() + Settings.LeadTime;
		const std::uint16_t BaseSequence = Random16();
		const std::uint32_t BaseTimestamp = Random32();

		for (Bot* Target : Active) {
			Target->Player->StepPrepare(BaseSequence, BaseTimestamp);
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
			for (Bot* Target : Active) {
				if (Target->Player->Status() != AudioPlayerStatus::Idle) {
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
					Active[Index]->Connection->DispatchAudio();
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
				for (Bot* Target : Active) {
					Target->Connection->DispatchAudio();
				}
			}

			const std::uint16_t Sequence = static_cast<std::uint16_t>(BaseSequence + Frame + 1);
			const std::uint32_t Timestamp = BaseTimestamp
				+ static_cast<std::uint32_t>((Frame + 1) * TimestampIncrement);

			for (Bot* Target : Active) {
				Target->Player->StepPrepare(Sequence, Timestamp);
			}
		}

		ReportSync();

		Playing = false;
		for (Bot* Target : Active) {
			Target->Connection->SetSpeaking(false);
			Target->Player->Stop(true);
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
		Stop();

		Connector.request_stop();
		if (Connector.joinable() && Connector.get_id() != std::this_thread::get_id()) {
			Connector.join();
		}

		for (std::unique_ptr<Bot>& Entry : Bots) {
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
		Stopping = true;
		Condition.notify_all();
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
