#include "Voice/Voice.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int Argc, char** Argv) {
	int Argument = 1;
	bool SelfBot = false;
	if (Argc > 1 && std::string(Argv[1]) == "--selfbot") {
		SelfBot = true;
		++Argument;
	}

	if (Argc < Argument + 4) {
		std::cout << "VoicePlay [--selfbot] <GuildId> <ChannelId> <File> <Token> [Token...]\n";
		return 1;
	}

	Voice::JoinConfig Config;
	Config.GuildId = Argv[Argument];
	Config.ChannelId = Argv[Argument + 1];

	std::vector<Voice::BotOptions> Bots;
	for (int Index = Argument + 3; Index < Argc; ++Index) {
		Voice::BotOptions Bot;
		Bot.Token = Argv[Index];
		Bot.Name = "Bot" + std::to_string(Index - Argument - 2);
		Bot.SelfBot = SelfBot;
		Bots.push_back(std::move(Bot));
	}

	Voice::PlaybackOptions Options;
	if (const char* Ffmpeg = std::getenv("FFMPEG_PATH")) {
		Options.FfmpegPath = Ffmpeg;
	}
	if (const char* Debug = std::getenv("VOICE_DEBUG")) {
		Options.Debug = std::string(Debug) == "1";
	}

	try {
		Voice::VoiceClient Client(std::move(Bots), std::move(Config), Options);

		Client.OnError = [](const std::string& Bot, const std::string& Message) {
			std::cerr << "[" << Bot << "] " << Message << "\n";
		};
		Client.OnDebug = [](const std::string& Message) {
			std::cout << Message << "\n";
		};

		if (!Client.Start()) {
			std::cerr << "Ready " << Client.ReadyCount() << "/" << Client.BotCount()
					  << " - " << Client.LastError() << "\n";
			return 1;
		}

		Client.PlayFile(Argv[Argument + 2]);
		std::cout << "Playing " << Argv[Argument + 2] << " on "
				  << Client.ReadyCount() << " bot(s)\n";
		Client.WaitForFinished();
		return 0;
	} catch (const std::exception& Exception) {
		std::cerr << Exception.what() << "\n";
		return 1;
	}
}
