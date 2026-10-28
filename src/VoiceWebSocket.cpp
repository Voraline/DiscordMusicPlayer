#include "Voice/VoiceWebSocket.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace Voice {

namespace {

std::wstring Utf8ToWide(const std::string& Text) {
	if (Text.empty()) {
		return {};
	}
	const int Size = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
	std::wstring Result(static_cast<std::size_t>(Size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Result.data(), Size);
	return Result;
}

std::int64_t NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::steady_clock::now().time_since_epoch()
	)
		.count();
}

std::int64_t SystemNowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::system_clock::now().time_since_epoch()
	)
		.count();
}

std::string WinHttpError(DWORD Code) {
	return "WinHTTP error " + std::to_string(Code);
}

}

VoiceWebSocket::VoiceWebSocket(std::string Url, int AckOpcode, int HeartbeatOpcode, bool Debug)
	: Url_(std::move(Url)), AckOpcode_(AckOpcode), HeartbeatOpcode_(HeartbeatOpcode), Debug_(Debug) {}

VoiceWebSocket::~VoiceWebSocket() {
	Destroy();
}

void VoiceWebSocket::Connect() {
	bool Expected = false;
	if (!Started_.compare_exchange_strong(Expected, true)) {
		return;
	}
	OpenThread_ = std::thread([this] { OpenLoop(); });
	HeartbeatThread_ = std::thread([this] { HeartbeatLoop(); });
}

bool VoiceWebSocket::OpenConnection() {
	std::string Url = Url_;
	bool Secure = true;
	if (Url.rfind("wss://", 0) == 0) {
		Url = "https://" + Url.substr(6);
	} else if (Url.rfind("ws://", 0) == 0) {
		Url = "http://" + Url.substr(5);
		Secure = false;
	}

	std::wstring WideUrl = Utf8ToWide(Url);

	URL_COMPONENTS Components{};
	Components.dwStructSize = sizeof(Components);
	Components.dwSchemeLength = static_cast<DWORD>(-1);
	Components.dwHostNameLength = static_cast<DWORD>(-1);
	Components.dwUrlPathLength = static_cast<DWORD>(-1);
	Components.dwExtraInfoLength = static_cast<DWORD>(-1);

	if (!WinHttpCrackUrl(WideUrl.data(), static_cast<DWORD>(WideUrl.size()), 0, &Components)) {
		Fail("WinHttpCrackUrl failed with error " + std::to_string(GetLastError()));
		return false;
	}

	std::wstring Host(Components.lpszHostName, Components.dwHostNameLength);
	std::wstring Path(Components.lpszUrlPath, Components.dwUrlPathLength);
	if (Components.dwExtraInfoLength > 0) {
		Path.append(Components.lpszExtraInfo, Components.dwExtraInfoLength);
	}
	if (Path.empty()) {
		Path = L"/";
	}

	HINTERNET Session = WinHttpOpen(
		L"Voice/1.0",
		WINHTTP_ACCESS_TYPE_NO_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0
	);
	if (!Session) {
		Fail("WinHttpOpen failed with error " + std::to_string(GetLastError()));
		return false;
	}
	Session_ = Session;
	WinHttpSetTimeouts(Session, 10000, 10000, 10000, 15000);

	const INTERNET_PORT Port = Components.nPort != 0
		? Components.nPort
		: static_cast<INTERNET_PORT>(Components.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80);

	HINTERNET Connection = WinHttpConnect(Session, Host.c_str(), Port, 0);
	if (!Connection) {
		Fail("WinHttpConnect failed with error " + std::to_string(GetLastError()));
		CleanupHandles();
		return false;
	}
	Connection_ = Connection;

	HINTERNET Request = WinHttpOpenRequest(
		Connection,
		L"GET",
		Path.c_str(),
		nullptr,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		Secure ? WINHTTP_FLAG_SECURE : 0
	);
	if (!Request) {
		Fail("WinHttpOpenRequest failed with error " + std::to_string(GetLastError()));
		CleanupHandles();
		return false;
	}
	Request_ = Request;

	if (!WinHttpSetOption(Request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
		Fail("WebSocket upgrade option failed with error " + std::to_string(GetLastError()));
		CleanupHandles();
		return false;
	}
	if (!WinHttpSendRequest(Request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
		Fail("WinHttpSendRequest failed with error " + std::to_string(GetLastError()));
		CleanupHandles();
		return false;
	}
	if (!WinHttpReceiveResponse(Request, nullptr)) {
		Fail("WinHttpReceiveResponse failed with error " + std::to_string(GetLastError()));
		CleanupHandles();
		return false;
	}

	HINTERNET Socket = WinHttpWebSocketCompleteUpgrade(Request, 0);
	if (!Socket) {
		Fail("WebSocket upgrade failed with error " + std::to_string(GetLastError()));
		CleanupHandles();
		return false;
	}

	Socket_ = Socket;
	WinHttpCloseHandle(Request);
	Request_ = nullptr;
	return true;
}

void VoiceWebSocket::OpenLoop() {
	if (Stop_) {
		CleanupHandles();
		return;
	}

	if (!OpenConnection() || Stop_) {
		const bool Failed = !Stop_;
		Open_ = false;
		CleanupHandles();
		if (Failed) {
			Fail("Failed to connect to " + Url_);
			if (OnClose) {
				OnClose(0);
			}
		}
		return;
	}

	Open_ = true;
	if (OnOpen) {
		OnOpen();
	}

	ReceiveLoop();

	Open_ = false;
	CleanupHandles();
}

void VoiceWebSocket::ReceiveLoop() {
	std::vector<std::uint8_t> Buffer(262144);
	std::string Message;
	bool Binary = false;

	while (!Stop_) {
		HINTERNET Socket = static_cast<HINTERNET>(Socket_.load());
		if (!Socket) {
			break;
		}

		DWORD BytesRead = 0;
		WINHTTP_WEB_SOCKET_BUFFER_TYPE Type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
		const DWORD Result = WinHttpWebSocketReceive(
			Socket,
			Buffer.data(),
			static_cast<DWORD>(Buffer.size()),
			&BytesRead,
			&Type
		);

		if (Result != ERROR_SUCCESS) {
			if (Result == ERROR_WINHTTP_TIMEOUT) {
				continue;
			}
			break;
		}

		if (Type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
			USHORT Status = 0;
			DWORD ReasonLength = 0;
			WinHttpWebSocketQueryCloseStatus(Socket, &Status, nullptr, 0, &ReasonLength);
			CloseCode_ = static_cast<int>(Status);
			break;
		}

		if (BytesRead > 0) {
			Message.append(reinterpret_cast<const char*>(Buffer.data()), BytesRead);
		}

		if (Type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE
			|| Type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
			Binary = true;
		}

		if (Type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
			|| Type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
			if (!Message.empty()) {
				if (Binary) {
					if (Message.size() >= 3) {
						const int Sequence = (static_cast<std::uint8_t>(Message[0]) << 8)
							| static_cast<std::uint8_t>(Message[1]);
						LastSequence_ = Sequence;
						const int Opcode = static_cast<std::uint8_t>(Message[2]);
						std::vector<std::uint8_t> Payload(
							Message.begin() + 3,
							Message.end()
						);
						if (Debug_ && OnDebug) {
							OnDebug(
								"<< binary op " + std::to_string(Opcode) + " seq "
								+ std::to_string(Sequence) + " ("
								+ std::to_string(Payload.size()) + " bytes)"
							);
						}
						if (OnBinary) {
							OnBinary(Opcode, std::move(Payload));
						}
					}
				} else {
					HandleMessage(Message);
				}
			}
			Message.clear();
			Binary = false;
		}
	}

	if (!Stop_ && OnClose) {
		OnClose(CloseCode_.load());
	}
}

void VoiceWebSocket::HandleMessage(const std::string& Message) {
	Json Packet = Json::parse(Message, nullptr, false);
	if (Packet.is_discarded() || !Packet.is_object()) {
		return;
	}

	if (Packet.contains("seq") && Packet["seq"].is_number_integer()) {
		LastSequence_ = Packet["seq"].get<std::int64_t>();
	}

	if (Debug_ && OnDebug) {
		const int Opcode = Packet.contains("op") && Packet["op"].is_number_integer()
			? Packet["op"].get<int>()
			: -1;
		std::string Line = "<< op " + std::to_string(Opcode);
		if (Packet.contains("t") && Packet["t"].is_string()) {
			Line += " " + Packet["t"].get<std::string>();
		}
		OnDebug(Line);
	}

	if (Packet.contains("op") && Packet["op"].is_number_integer()
		&& Packet["op"].get<int>() == AckOpcode_) {
		const std::int64_t Now = NowMs();
		LastHeartbeatAck_ = Now;
		MissedHeartbeats_ = 0;
		const std::int64_t Sent = LastHeartbeatSend_.load();
		if (Sent != 0) {
			PingMs_ = static_cast<int>(Now - Sent);
		}
	}

	if (OnPacket) {
		OnPacket(Packet);
	}
}

void VoiceWebSocket::HeartbeatLoop() {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

	while (!Stop_) {
		const int Interval = HeartbeatInterval_.load();
		if (Interval <= 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			continue;
		}

		int Remaining = Interval;
		while (Remaining > 0 && !Stop_) {
			const int Step = std::min(Remaining, 25);
			std::this_thread::sleep_for(std::chrono::milliseconds(Step));
			Remaining -= Step;
		}
		if (Stop_) {
			break;
		}

		if (LastHeartbeatSend_.load() != 0 && MissedHeartbeats_.load() >= 3) {
			Close(4000);
			break;
		}

		SendHeartbeat();
	}
}

bool VoiceWebSocket::SendPacket(const Json& Packet) {
	if (Debug_ && OnDebug) {
		const int Opcode = Packet.contains("op") && Packet["op"].is_number_integer()
			? Packet["op"].get<int>()
			: -1;
		OnDebug(">> op " + std::to_string(Opcode));
	}
	return SendText(Packet.dump());
}

bool VoiceWebSocket::SendText(const std::string& Text) {
	if (!Open_.load() || Stop_.load()) {
		return false;
	}

	std::lock_guard Lock(SendMutex_);
	HINTERNET Socket = static_cast<HINTERNET>(Socket_.load());
	if (!Socket || !Open_.load()) {
		return false;
	}

	const DWORD Result = WinHttpWebSocketSend(
		Socket,
		WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
		const_cast<char*>(Text.data()),
		static_cast<DWORD>(Text.size())
	);
	if (Result != ERROR_SUCCESS) {
		Fail(WinHttpError(Result));
		return false;
	}
	return true;
}

bool VoiceWebSocket::SendBinary(int Opcode, std::span<const std::uint8_t> Payload) {
	if (!Open_.load() || Stop_.load()) {
		return false;
	}

	std::lock_guard Lock(SendMutex_);
	HINTERNET Socket = static_cast<HINTERNET>(Socket_.load());
	if (!Socket || !Open_.load()) {
		return false;
	}

	std::vector<std::uint8_t> Frame;
	Frame.reserve(Payload.size() + 1);
	Frame.push_back(static_cast<std::uint8_t>(Opcode & 0xFF));
	Frame.insert(Frame.end(), Payload.begin(), Payload.end());

	if (Debug_ && OnDebug) {
		OnDebug(
			">> binary " + std::to_string(Opcode) + " (" + std::to_string(Payload.size())
			+ " bytes)"
		);
	}

	const DWORD Result = WinHttpWebSocketSend(
		Socket,
		WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
		Frame.data(),
		static_cast<DWORD>(Frame.size())
	);
	if (Result != ERROR_SUCCESS) {
		Fail(WinHttpError(Result));
		return false;
	}
	return true;
}

void VoiceWebSocket::SetHeartbeatInterval(int Milliseconds) {
	const int Previous = HeartbeatInterval_.exchange(Milliseconds);
	if (ImmediateHeartbeat && Milliseconds > 0 && Previous <= 0) {
		SendHeartbeat();
	}
}

void VoiceWebSocket::SendHeartbeat() {
	Json Data;
	if (ObjectHeartbeat) {
		Data = Json::object();
		Data["t"] = SystemNowMs();
		Data["seq_ack"] = LastSequence_.load();
	} else {
		Data = HeartbeatPayload ? HeartbeatPayload() : Json(nullptr);
	}

	LastHeartbeatSend_ = NowMs();
	MissedHeartbeats_++;

	Json Packet;
	Packet["op"] = HeartbeatOpcode_;
	Packet["d"] = std::move(Data);

	SendPacket(Packet);
}

void VoiceWebSocket::Close(int Code) {
	Stop_ = true;
	HINTERNET Socket = static_cast<HINTERNET>(Socket_.exchange(nullptr));
	if (Socket) {
		WinHttpWebSocketClose(Socket, static_cast<USHORT>(Code), nullptr, 0);
		WinHttpCloseHandle(Socket);
	}
}

void VoiceWebSocket::Destroy() {
	Stop_ = true;
	HINTERNET Socket = static_cast<HINTERNET>(Socket_.exchange(nullptr));
	if (Socket) {
		WinHttpCloseHandle(Socket);
	}
	JoinThreads();
	CleanupHandles();
}

void VoiceWebSocket::JoinThreads() {
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

	Join(OpenThread_);
	Join(HeartbeatThread_);
}

void VoiceWebSocket::CleanupHandles() {
	if (HINTERNET Socket = static_cast<HINTERNET>(Socket_.exchange(nullptr))) {
		WinHttpCloseHandle(Socket);
	}
	if (HINTERNET Request = static_cast<HINTERNET>(Request_.exchange(nullptr))) {
		WinHttpCloseHandle(Request);
	}
	if (HINTERNET Connection = static_cast<HINTERNET>(Connection_.exchange(nullptr))) {
		WinHttpCloseHandle(Connection);
	}
	if (HINTERNET Session = static_cast<HINTERNET>(Session_.exchange(nullptr))) {
		WinHttpCloseHandle(Session);
	}
}

void VoiceWebSocket::Fail(const std::string& Message) {
	if (OnError) {
		OnError(Message);
	}
}

std::optional<int> VoiceWebSocket::Ping() const {
	const int Value = PingMs_.load();
	if (Value < 0) {
		return std::nullopt;
	}
	return Value;
}

bool VoiceWebSocket::IsOpen() const {
	return Open_.load();
}

}
