#include "Voice/VoiceUdpSocket.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>

namespace Voice {

namespace {

constexpr std::uintptr_t InvalidSocketValue = ~static_cast<std::uintptr_t>(0);
constexpr std::size_t DiscoverySize = 74;
constexpr std::size_t KeepAliveSize = 8;

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket InvalidNativeSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket InvalidNativeSocket = -1;
#endif

#if defined(_WIN32)
std::once_flag WinsockOnce;
#endif

void EnsureWinsock() {
#if defined(_WIN32)
	std::call_once(WinsockOnce, [] {
		WSADATA Data{};
		WSAStartup(MAKEWORD(2, 2), &Data);
	});
#endif
}

int LastSocketError() {
#if defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

void CloseNativeSocket(NativeSocket Socket) {
#if defined(_WIN32)
	closesocket(Socket);
#else
	::close(Socket);
#endif
}

bool IsTransientSocketError(int Error) {
#if defined(_WIN32)
	return Error == WSAETIMEDOUT || Error == WSAEWOULDBLOCK;
#else
	return Error == EAGAIN || Error == EWOULDBLOCK || Error == EINTR;
#endif
}

bool ParseIPv4(const std::string& Text, in_addr& Address) {
#if defined(_WIN32)
	return InetPtonA(AF_INET, Text.c_str(), &Address) == 1;
#else
	return inet_pton(AF_INET, Text.c_str(), &Address) == 1;
#endif
}

std::int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::steady_clock::now().time_since_epoch()
	)
		.count();
}

std::uint16_t ReadBig16(const std::uint8_t* Data) {
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(Data[0]) << 8) | Data[1]);
}

std::uint32_t ReadLittle32(const std::uint8_t* Data) {
	return static_cast<std::uint32_t>(Data[0])
		| (static_cast<std::uint32_t>(Data[1]) << 8)
		| (static_cast<std::uint32_t>(Data[2]) << 16)
		| (static_cast<std::uint32_t>(Data[3]) << 24);
}

void WriteBig16(std::uint8_t* Data, std::uint16_t Value) {
	Data[0] = static_cast<std::uint8_t>((Value >> 8) & 0xFF);
	Data[1] = static_cast<std::uint8_t>(Value & 0xFF);
}

void WriteBig32(std::uint8_t* Data, std::uint32_t Value) {
	Data[0] = static_cast<std::uint8_t>((Value >> 24) & 0xFF);
	Data[1] = static_cast<std::uint8_t>((Value >> 16) & 0xFF);
	Data[2] = static_cast<std::uint8_t>((Value >> 8) & 0xFF);
	Data[3] = static_cast<std::uint8_t>(Value & 0xFF);
}

void WriteLittle32(std::uint8_t* Data, std::uint32_t Value) {
	Data[0] = static_cast<std::uint8_t>(Value & 0xFF);
	Data[1] = static_cast<std::uint8_t>((Value >> 8) & 0xFF);
	Data[2] = static_cast<std::uint8_t>((Value >> 16) & 0xFF);
	Data[3] = static_cast<std::uint8_t>((Value >> 24) & 0xFF);
}

}

VoiceUdpSocket::VoiceUdpSocket(SocketConfig Remote, bool Debug)
	: Remote_(std::move(Remote)), Debug_(Debug) {}

VoiceUdpSocket::~VoiceUdpSocket() {
	Destroy();
}

bool VoiceUdpSocket::Open() {
	if (Socket_.load() != InvalidSocketValue) {
		return true;
	}

	EnsureWinsock();

	NativeSocket S = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (S == InvalidNativeSocket) {
		if (OnError) {
			OnError("Failed to create UDP socket");
		}
		return false;
	}

	sockaddr_in Address{};
	Address.sin_family = AF_INET;
	Address.sin_port = htons(Remote_.Port);
	if (!ParseIPv4(Remote_.Ip, Address.sin_addr)) {
		CloseNativeSocket(S);
		if (OnError) {
			OnError("Invalid voice server address " + Remote_.Ip);
		}
		return false;
	}

	if (connect(S, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) != 0) {
		CloseNativeSocket(S);
		if (OnError) {
			OnError("Failed to connect UDP socket");
		}
		return false;
	}

	int BufferSize = 1 << 20;
	setsockopt(S, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&BufferSize), sizeof(BufferSize));
	setsockopt(S, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&BufferSize), sizeof(BufferSize));

#if !defined(_WIN32)
	int SocketPriority = 6;
	setsockopt(S, SOL_SOCKET, SO_PRIORITY, &SocketPriority, sizeof(SocketPriority));
	int Tos = 0xB8;
	setsockopt(S, IPPROTO_IP, IP_TOS, &Tos, sizeof(Tos));
#endif

#if defined(_WIN32)
	DWORD Timeout = 250;
#else
	timeval Timeout{};
	Timeout.tv_sec = 0;
	Timeout.tv_usec = 250000;
#endif
	setsockopt(S, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&Timeout), sizeof(Timeout));

	Stop_ = false;
	Socket_ = static_cast<std::uintptr_t>(S);
	ReceiveThread_ = std::thread([this] { ReceiveLoop(); });
	KeepAliveThread_ = std::thread([this] { KeepAliveLoop(); });
	return true;
}

bool VoiceUdpSocket::Send(std::span<const std::uint8_t> Data) {
	const NativeSocket S = static_cast<NativeSocket>(Socket_.load());
	if (S == InvalidNativeSocket) {
		return false;
	}

	const int Result = send(S, reinterpret_cast<const char*>(Data.data()), static_cast<int>(Data.size()), 0);
	if (Result < 0) {
		if (OnError) {
			OnError("UDP send failed with error " + std::to_string(LastSocketError()));
		}
		return false;
	}
	return true;
}

bool VoiceUdpSocket::Send(const std::vector<std::uint8_t>& Data) {
	return Send(std::span<const std::uint8_t>(Data.data(), Data.size()));
}

void VoiceUdpSocket::ReceiveLoop() {
	std::vector<std::uint8_t> Buffer(2048);

	while (!Stop_) {
		const NativeSocket S = static_cast<NativeSocket>(Socket_.load());
		if (S == InvalidNativeSocket) {
			break;
		}

		const int Received = recv(S, reinterpret_cast<char*>(Buffer.data()), static_cast<int>(Buffer.size()), 0);
		if (Received < 0) {
			if (Stop_) {
				break;
			}
			if (IsTransientSocketError(LastSocketError())) {
				continue;
			}
			break;
		}
		if (Received == 0) {
			continue;
		}

		HandleDatagram(Buffer.data(), static_cast<std::size_t>(Received));
	}

	if (!Stop_ && OnClose) {
		OnClose();
	}
}

void VoiceUdpSocket::HandleDatagram(const std::uint8_t* Data, std::size_t Size) {
	if (Size == KeepAliveSize) {
		const std::uint32_t Counter = ReadLittle32(Data);
		bool Matched = false;
		int Ping = -1;
		{
			std::lock_guard Lock(Mutex_);
			for (std::size_t Index = 0; Index < KeepAlives_.size(); ++Index) {
				if (KeepAlives_[Index].Value == Counter) {
					Ping = static_cast<int>(NowMs() - KeepAlives_[Index].Timestamp);
					PingMs_ = Ping;
					KeepAlives_.erase(
						KeepAlives_.begin(),
						KeepAlives_.begin() + static_cast<std::ptrdiff_t>(Index + 1)
					);
					Matched = true;
					break;
				}
			}
		}
		if (Matched && Debug_ && OnDebug) {
			OnDebug(
				"UDP keepalive ack counter " + std::to_string(Counter) + " ping " + std::to_string(Ping)
			);
		}
	} else if (Size >= DiscoverySize && ReadBig16(Data) == 2) {
		const std::uint8_t* Start = Data + 8;
		const std::uint8_t* End = static_cast<const std::uint8_t*>(std::memchr(Start, 0, Size - 8));
		if (End) {
			const std::string Ip(reinterpret_cast<const char*>(Start), reinterpret_cast<const char*>(End));
			in_addr Parsed{};
			if (ParseIPv4(Ip, Parsed)) {
				SocketConfig Config{Ip, ReadBig16(Data + Size - 2)};
				{
					std::lock_guard Lock(Mutex_);
					DiscoveryResult_ = std::move(Config);
				}
				DiscoveryCv_.notify_all();
			}
		}
	}

	if (OnMessage) {
		OnMessage(std::vector<std::uint8_t>(Data, Data + Size));
	}
}

std::optional<SocketConfig> VoiceUdpSocket::PerformIPDiscovery(
	std::uint32_t Ssrc,
	std::chrono::milliseconds Timeout
) {
	{
		std::lock_guard Lock(Mutex_);
		DiscoveryResult_.reset();
	}

	std::uint8_t Packet[DiscoverySize]{};
	WriteBig16(Packet, 1);
	WriteBig16(Packet + 2, 70);
	WriteBig32(Packet + 4, Ssrc);

	if (!Send(std::span<const std::uint8_t>(Packet, sizeof(Packet)))) {
		return std::nullopt;
	}

	std::unique_lock Lock(Mutex_);
	const bool Ready = DiscoveryCv_.wait_for(Lock, Timeout, [this] {
		return DiscoveryResult_.has_value() || Stop_.load();
	});
	if (!Ready) {
		return std::nullopt;
	}
	return DiscoveryResult_;
}

void VoiceUdpSocket::KeepAliveLoop() {
	while (!Stop_) {
		for (int Step = 0; Step < 50 && !Stop_; ++Step) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (Stop_) {
			break;
		}

		const std::uint32_t Counter = KeepAliveCounter_.fetch_add(1);
		{
			std::lock_guard Lock(Mutex_);
			if (KeepAlives_.size() >= 64) {
				KeepAlives_.clear();
				if (Debug_ && OnDebug && !KeepAliveWarned_) {
					KeepAliveWarned_ = true;
					OnDebug("UDP keepalive responses not seen; continuing");
				}
			}
			KeepAlives_.push_back({Counter, NowMs()});
		}

		std::uint8_t Packet[KeepAliveSize]{};
		WriteLittle32(Packet, Counter);
		if (!Send(std::span<const std::uint8_t>(Packet, sizeof(Packet)))) {
			break;
		}
	}
}

void VoiceUdpSocket::Destroy() {
	Stop_ = true;
	const NativeSocket S = static_cast<NativeSocket>(Socket_.exchange(InvalidSocketValue));
	if (S != InvalidNativeSocket) {
		CloseNativeSocket(S);
	}
	DiscoveryCv_.notify_all();
	JoinThreads();
}

void VoiceUdpSocket::JoinThreads() {
	auto Join = [](std::thread& Thread) {
		if (!Thread.joinable()) {
			return;
		}
		if (Thread.get_id() == std::this_thread::get_id()) {
			Thread.detach();
			return;
		}
		Thread.join();
	};

	Join(ReceiveThread_);
	Join(KeepAliveThread_);
}

std::optional<int> VoiceUdpSocket::Ping() const {
	const int Value = PingMs_.load();
	if (Value < 0) {
		return std::nullopt;
	}
	return Value;
}

bool VoiceUdpSocket::IsOpen() const {
	return Socket_.load() != InvalidSocketValue;
}

}
