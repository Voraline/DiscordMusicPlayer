#include "Voice/VoiceClient.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "LinuxRealtime.h"
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
constexpr std::size_t SyncStartFrame = 20;
constexpr std::size_t SyncSampleCount = 200;
constexpr std::chrono::microseconds RealtimeSpinWindow{250};
constexpr std::chrono::milliseconds FallbackSpinWindow{1};

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

}

struct VoiceClient::Impl {
	struct Bot {
		BotOptions Options;
		std::unique_ptr<GatewayClient> Gateway;
		std::unique_ptr<VoiceConnection> Connection;
		std::unique_ptr<AudioPlayer> Player;
		std::shared_ptr<AudioResource> Resource;
		std::atomic<bool> Ready{false};
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

	Impl(
		VoiceClient* OwnerPointer,
		std::vector<BotOptions> InputBots,
		JoinConfig ConfigValue,
		PlaybackOptions PlaybackSettings
	)
		: Owner(OwnerPointer),
		  Config(std::move(ConfigValue)),
		  Settings(std::move(PlaybackSettings)) {
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
		Bots.clear();
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
		if (Increment) {
			Debug("[" + Target->Options.Name + "] voice ready");
		}
		Condition.notify_all();
	}

	void HandleConnectionClosed(Bot* Target, int Code) {
		const bool WasReady = Target->Ready.exchange(false);
		{
			std::lock_guard Lock(Mutex);
			if (WasReady) {
				if (ReadyCount > 0) {
					--ReadyCount;
				}
			} else {
				++FailedCount;
			}
		}
		if (Code == 4006) {
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
		const std::vector<int> Cpus = Linux::AllowedCpus();
		const bool Locked = Linux::LockMemory();

		const std::size_t Count = Active.size();
		const bool Parallel = Count > 1 && Count >= Settings.ParallelThreshold && Cpus.size() >= Count;
		const bool MeasureSync = Settings.Debug && Count > 1;

		const auto FrameDuration = std::chrono::milliseconds(FrameMilliseconds);
		const auto StartTime = std::chrono::steady_clock::now() + Settings.LeadTime;
		const std::uint16_t BaseSequence = Random16();
		const std::uint32_t BaseTimestamp = Random32();
		const std::size_t Warmup = static_cast<std::size_t>(std::max(0, Settings.WarmupFrames));
		const std::size_t MaximumFrames = Warmup + Audio->Frames.size()
			+ static_cast<std::size_t>(Settings.SilencePaddingFrames) + 100;

		const auto DeadlineOf = [&](std::size_t Frame) {
			return StartTime + FrameDuration * static_cast<std::int64_t>(Frame);
		};

		const auto PrepareFrame = [&](Bot* Target, std::size_t Frame) {
			const std::uint16_t Sequence = static_cast<std::uint16_t>(BaseSequence + Frame);
			const std::uint32_t Timestamp = BaseTimestamp
				+ static_cast<std::uint32_t>(Frame * TimestampIncrement);
			if (Frame < Warmup) {
				Target->Connection->PrepareAudioPacketFast(
					AudioResource::SilenceFrame(),
					Sequence,
					Timestamp
				);
			} else {
				Target->Player->StepPrepare(Sequence, Timestamp);
			}
		};

		const auto LateFrames = [&](std::size_t Frame, std::chrono::steady_clock::time_point Now) {
			const auto Lag = Now - DeadlineOf(Frame);
			if (Lag < FrameDuration) {
				return std::size_t{0};
			}
			return std::min(
				static_cast<std::size_t>(Lag / FrameDuration),
				MaximumFrames - 1 - Frame
			);
		};

		std::array<long long, SyncSampleCount> SyncSamples{};
		std::size_t SyncCount = 0;
		long long MaxSkewNs = 0;
		bool SyncReported = false;

		const auto ReportSync = [&] {
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

		if (Parallel) {
			Debug(
				"[SYNC] mode=parallel workers=" + std::to_string(Count)
				+ " locked=" + std::string(Locked ? "1" : "0")
			);

			std::vector<long long> Stamps(MeasureSync ? Count * SyncSampleCount : 0, 0);
			std::vector<std::thread> Workers;
			Workers.reserve(Count);

			for (std::size_t Index = 0; Index < Count; ++Index) {
				Workers.emplace_back([&, Index] {
					Bot* Target = Active[Index];
					Linux::PinCurrentThread(Cpus[Cpus.size() - 1 - Index]);
					const std::chrono::nanoseconds Spin = Linux::ElevateCurrentThread()
						? RealtimeSpinWindow
						: FallbackSpinWindow;

					std::size_t Frame = 0;
					PrepareFrame(Target, Frame);
					while (Frame < MaximumFrames) {
						if (Target->Player->Status() == AudioPlayerStatus::Idle || Stopping.load()) {
							break;
						}

						const auto Now = Linux::WaitUntil(DeadlineOf(Frame), Spin, Stopping);
						if (Stopping.load()) {
							break;
						}

						const std::size_t Skip = LateFrames(Frame, Now);
						for (std::size_t Ahead = 1; Ahead <= Skip; ++Ahead) {
							PrepareFrame(Target, Frame + Ahead);
						}
						Frame += Skip;

						if (MeasureSync && Frame >= SyncStartFrame
							&& Frame < SyncStartFrame + SyncSampleCount) [[unlikely]] {
							Stamps[Index * SyncSampleCount + (Frame - SyncStartFrame)]
								= std::chrono::duration_cast<std::chrono::nanoseconds>(
									  std::chrono::steady_clock::now().time_since_epoch()
								)
									  .count();
						}
						Target->Connection->DispatchAudio();

						++Frame;
						PrepareFrame(Target, Frame);
					}
				});
			}

			for (std::thread& Worker : Workers) {
				Worker.join();
			}

			if (MeasureSync) {
				for (std::size_t Slot = 0; Slot < SyncSampleCount; ++Slot) {
					long long Low = std::numeric_limits<long long>::max();
					long long High = 0;
					bool Complete = true;
					for (std::size_t Index = 0; Index < Count; ++Index) {
						const long long Value = Stamps[Index * SyncSampleCount + Slot];
						if (Value == 0) {
							Complete = false;
							break;
						}
						Low = std::min(Low, Value);
						High = std::max(High, Value);
					}
					if (Complete) {
						SyncSamples[SyncCount++] = High - Low;
						MaxSkewNs = std::max(MaxSkewNs, High - Low);
					}
				}
			}
			ReportSync();
		} else {
			const bool Pinned = !Cpus.empty() && Linux::PinCurrentThread(Cpus.back());
			const bool Realtime = Linux::ElevateCurrentThread();
			const std::chrono::nanoseconds Spin = Realtime ? RealtimeSpinWindow : FallbackSpinWindow;
			Debug(
				"[SYNC] mode=serial realtime=" + std::string(Realtime ? "1" : "0")
				+ " pinned=" + std::string(Pinned ? "1" : "0")
				+ " locked=" + std::string(Locked ? "1" : "0")
			);

			for (Bot* Target : Active) {
				PrepareFrame(Target, 0);
			}

			std::size_t Frame = 0;
			while (Frame < MaximumFrames) {
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

				const auto Now = Linux::WaitUntil(DeadlineOf(Frame), Spin, Stopping);
				if (Stopping.load()) {
					break;
				}

				const std::size_t Skip = LateFrames(Frame, Now);
				if (Skip > 0) {
					for (Bot* Target : Active) {
						for (std::size_t Ahead = 1; Ahead <= Skip; ++Ahead) {
							PrepareFrame(Target, Frame + Ahead);
						}
					}
					Frame += Skip;
				}

				const std::size_t Offset = Frame % Count;
				if (MeasureSync && Frame >= SyncStartFrame && SyncCount < SyncSampleCount) [[unlikely]] {
					const auto First = std::chrono::steady_clock::now();
					auto Last = First;
					for (std::size_t Step = 0; Step < Count; ++Step) {
						Last = std::chrono::steady_clock::now();
						Active[(Offset + Step) % Count]->Connection->DispatchAudio();
					}
					const long long SkewNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
						Last - First
					)
						.count();
					MaxSkewNs = std::max(MaxSkewNs, SkewNs);
					SyncSamples[SyncCount++] = SkewNs;
					if (SyncCount == SyncSamples.size()) {
						ReportSync();
					}
				} else {
					for (std::size_t Step = 0; Step < Count; ++Step) {
						Active[(Offset + Step) % Count]->Connection->DispatchAudio();
					}
				}

				++Frame;
				for (Bot* Target : Active) {
					PrepareFrame(Target, Frame);
				}
			}

			ReportSync();
		}

		Playing = false;
		for (Bot* Target : Active) {
			Target->Connection->SetSpeaking(false);
			Target->Player->Stop(true);
		}

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
