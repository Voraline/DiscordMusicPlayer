#include "Voice/OpusCodec.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include <opus.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Voice {

namespace {

constexpr int SampleRate = 48000;
constexpr int Channels = 2;
constexpr int FrameSamples = 960;
constexpr int FrameBytes = FrameSamples * Channels * static_cast<int>(sizeof(std::int16_t));
constexpr std::size_t ReadBufferSize = 16384;

#if defined(_WIN32)

std::wstring Utf8ToWide(const std::string& Text) {
	if (Text.empty()) {
		return {};
	}
	const int Size = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
	std::wstring Result(static_cast<std::size_t>(Size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Result.data(), Size);
	return Result;
}

struct FfmpegProcess {
	HANDLE Process = nullptr;
	HANDLE Read = nullptr;
	HANDLE Null = nullptr;

	~FfmpegProcess() {
		if (Read) {
			CloseHandle(Read);
			Read = nullptr;
		}
		if (Null) {
			CloseHandle(Null);
			Null = nullptr;
		}
		if (Process) {
			TerminateProcess(Process, 1);
			CloseHandle(Process);
			Process = nullptr;
		}
	}

	bool Start(const std::wstring& CommandLine) {
		SECURITY_ATTRIBUTES Attributes{};
		Attributes.nLength = sizeof(Attributes);
		Attributes.bInheritHandle = TRUE;

		HANDLE WritePipe = nullptr;
		if (!CreatePipe(&Read, &WritePipe, &Attributes, 0)) {
			return false;
		}
		SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);

		Null = CreateFileW(
			L"NUL",
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			&Attributes,
			OPEN_EXISTING,
			0,
			nullptr
		);
		if (Null == INVALID_HANDLE_VALUE) {
			Null = nullptr;
			CloseHandle(WritePipe);
			return false;
		}

		STARTUPINFOW Startup{};
		Startup.cb = sizeof(Startup);
		Startup.dwFlags = STARTF_USESTDHANDLES;
		Startup.hStdInput = Null;
		Startup.hStdOutput = WritePipe;
		Startup.hStdError = Null;

		PROCESS_INFORMATION Information{};
		std::wstring MutableCommand = CommandLine;

		const BOOL Created = CreateProcessW(
			nullptr,
			MutableCommand.data(),
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW,
			nullptr,
			nullptr,
			&Startup,
			&Information
		);

		CloseHandle(WritePipe);

		if (!Created) {
			return false;
		}

		CloseHandle(Information.hThread);
		Process = Information.hProcess;
		return true;
	}

	int Wait() {
		if (!Process) {
			return 1;
		}
		WaitForSingleObject(Process, INFINITE);
		DWORD ExitCode = 1;
		GetExitCodeProcess(Process, &ExitCode);
		CloseHandle(Process);
		Process = nullptr;
		return static_cast<int>(ExitCode);
	}
};

#else

std::string ShellQuote(const std::string& Text) {
	std::string Result;
	Result.reserve(Text.size() + 2);
	Result.push_back('\'');
	for (char Character : Text) {
		if (Character == '\'') {
			Result += "'\\''";
		} else {
			Result.push_back(Character);
		}
	}
	Result.push_back('\'');
	return Result;
}

struct PipeProcess {
	FILE* Pipe = nullptr;

	~PipeProcess() {
		if (Pipe) {
			pclose(Pipe);
			Pipe = nullptr;
		}
	}

	int Wait() {
		if (!Pipe) {
			return 1;
		}
		const int Status = pclose(Pipe);
		Pipe = nullptr;
		if (WIFEXITED(Status)) {
			return WEXITSTATUS(Status);
		}
		return Status;
	}
};

#endif

}

std::shared_ptr<EncodedAudio> OpusCodec::EncodeFile(
	const std::filesystem::path& File,
	const EncoderOptions& Options
) {
#if defined(_WIN32)
	const std::wstring Command = L"\"" + Utf8ToWide(Options.FfmpegPath)
		+ L"\" -v error -nostdin -i \"" + File.wstring()
		+ L"\" -vn -sn -dn -f s16le -ac 2 -ar 48000 - 2>NUL";

	FfmpegProcess ProcessHandle;
	if (!ProcessHandle.Start(Command)) {
		throw std::runtime_error("Failed to start FFmpeg");
	}
#else
	const std::string Command = ShellQuote(Options.FfmpegPath)
		+ " -v error -nostdin -i " + ShellQuote(File.string())
		+ " -vn -sn -dn -f s16le -ac 2 -ar 48000 - 2>/dev/null";

	PipeProcess ProcessHandle;
	ProcessHandle.Pipe = popen(Command.c_str(), "r");
	if (!ProcessHandle.Pipe) {
		throw std::runtime_error("Failed to start FFmpeg");
	}
#endif

	int Error = 0;
	OpusEncoder* Encoder = opus_encoder_create(SampleRate, Channels, OPUS_APPLICATION_AUDIO, &Error);
	if (Error != OPUS_OK || !Encoder) {
		throw std::runtime_error("Failed to create Opus encoder");
	}

	opus_encoder_ctl(Encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(Options.Bitrate)));
	opus_encoder_ctl(Encoder, OPUS_SET_COMPLEXITY(Options.Complexity));
	opus_encoder_ctl(Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

	auto Audio = std::make_shared<EncodedAudio>();
	Audio->SampleRate = SampleRate;
	Audio->Channels = Channels;
	Audio->FrameDurationMs = 20;

	std::array<std::uint8_t, ReadBufferSize> Buffer{};
	std::array<std::int16_t, FrameSamples * Channels> Samples{};
	std::array<std::uint8_t, 4000> Packet{};
	std::vector<std::uint8_t> Pending;
	Pending.reserve(ReadBufferSize * 2);

	while (true) {
		std::size_t Read = 0;
#if defined(_WIN32)
		DWORD BytesRead = 0;
		if (!ReadFile(ProcessHandle.Read, Buffer.data(), static_cast<DWORD>(Buffer.size()), &BytesRead, nullptr)
			|| BytesRead == 0) {
			break;
		}
		Read = static_cast<std::size_t>(BytesRead);
#else
		Read = std::fread(Buffer.data(), 1, Buffer.size(), ProcessHandle.Pipe);
		if (Read == 0) {
			break;
		}
#endif

		Pending.insert(Pending.end(), Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Read));

		std::size_t Offset = 0;
		while (Pending.size() - Offset >= static_cast<std::size_t>(FrameBytes)) {
			std::memcpy(Samples.data(), Pending.data() + Offset, FrameBytes);
			const int Encoded = opus_encode(
				Encoder,
				Samples.data(),
				FrameSamples,
				Packet.data(),
				static_cast<opus_int32>(Packet.size())
			);
			if (Encoded < 0) {
				opus_encoder_destroy(Encoder);
				throw std::runtime_error("Opus encoding failed");
			}
			Audio->Frames.emplace_back(Packet.begin(), Packet.begin() + Encoded);
			Offset += static_cast<std::size_t>(FrameBytes);
		}

		if (Offset > 0) {
			Pending.erase(Pending.begin(), Pending.begin() + static_cast<std::ptrdiff_t>(Offset));
		}
	}

	const int ExitCode = ProcessHandle.Wait();
	opus_encoder_destroy(Encoder);

	if (Audio->Frames.empty()) {
		throw std::runtime_error(
			"No audio decoded from " + File.string() + " (FFmpeg exit " + std::to_string(ExitCode) + ")"
		);
	}

	return Audio;
}

}
