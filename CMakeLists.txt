cmake_minimum_required(VERSION 3.24)
project(Voice LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(MLSPP CONFIG REQUIRED)

FetchContent_Declare(
	Monocypher
	GIT_REPOSITORY https://github.com/LoupVaillant/Monocypher.git
	GIT_TAG 4.0.2
	GIT_SHALLOW TRUE
)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
FetchContent_Declare(
	Opus
	GIT_REPOSITORY https://github.com/xiph/opus.git
	GIT_TAG v1.5.2
	GIT_SHALLOW TRUE
)

if(WIN32)
	set(VOICE_WEBSOCKET_SOURCE src/VoiceWebSocket.cpp)
	set(VOICE_PLATFORM_LIBS winhttp ws2_32 winmm bcrypt crypt32 avrt)
	set(VOICE_PLATFORM_DEFINITIONS
		_WIN32_WINNT=0x0A00
		WIN32_LEAN_AND_MEAN
		NOMINMAX
		_CRT_SECURE_NO_WARNINGS
	)
else()
	set(USE_TLS ON CACHE BOOL "" FORCE)
	set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
	set(USE_ZLIB OFF CACHE BOOL "" FORCE)
	set(USE_WS OFF CACHE BOOL "" FORCE)
	set(USE_TEST OFF CACHE BOOL "" FORCE)
	set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
	set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		IXWebSocket
		GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
		GIT_TAG 0ec87b6f6caf8fab32b85e4324bd1b225821ed09
	)
	FetchContent_MakeAvailable(IXWebSocket)

	set(VOICE_WEBSOCKET_SOURCE src/posix/VoiceWebSocket.cpp)
	set(VOICE_PLATFORM_LIBS ixwebsocket OpenSSL::SSL OpenSSL::Crypto pthread dl m)
	set(VOICE_PLATFORM_DEFINITIONS "")
endif()

FetchContent_MakeAvailable(Opus Monocypher)

add_subdirectory(third_party/libdave)
target_compile_definitions(libdave PUBLIC DAVE_STATIC)

add_library(Voice
	${monocypher_SOURCE_DIR}/src/monocypher.c
	src/Secretbox.cpp
	src/DaveApiStatic.cpp
	src/DaveSession.cpp
	${VOICE_WEBSOCKET_SOURCE}
	src/VoiceUdpSocket.cpp
	src/Networking.cpp
	src/GatewayClient.cpp
	src/AudioResource.cpp
	src/AudioPlayer.cpp
	src/OpusCodec.cpp
	src/VoiceConnection.cpp
	src/VoiceClient.cpp
)

target_include_directories(Voice PUBLIC include PRIVATE ${monocypher_SOURCE_DIR}/src)
if(NOT WIN32)
	target_include_directories(Voice BEFORE PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/posix)
endif()

if(VOICE_PLATFORM_DEFINITIONS)
	target_compile_definitions(Voice PUBLIC ${VOICE_PLATFORM_DEFINITIONS})
endif()

target_link_libraries(Voice PUBLIC
	nlohmann_json::nlohmann_json
	libdave
	opus
	${VOICE_PLATFORM_LIBS}
)

if(MSVC)
	target_compile_options(Voice PRIVATE
		/W4
		/permissive-
		/EHsc
		$<$<CONFIG:Release>:/O2>
		$<$<CONFIG:Release>:/Oi>
		$<$<CONFIG:Release>:/Ot>
		$<$<CONFIG:Release>:/GL>
	)
	target_link_options(Voice PRIVATE $<$<CONFIG:Release>:/LTCG>)
else()
	target_compile_options(Voice PRIVATE -O3 -pthread)
	target_link_options(Voice PRIVATE -pthread)
endif()

add_executable(VoicePlay examples/Play.cpp)
target_link_libraries(VoicePlay PRIVATE Voice)

add_executable(VoiceDaveTest tests/DaveTest.cpp)
target_include_directories(VoiceDaveTest PRIVATE src)
target_link_libraries(VoiceDaveTest PRIVATE Voice)

if(MSVC)
	target_compile_options(VoicePlay PRIVATE /W4 /permissive- $<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/GL>)
	target_link_options(VoicePlay PRIVATE $<$<CONFIG:Release>:/LTCG>)
endif()
