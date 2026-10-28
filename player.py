#!/usr/bin/env python3
import argparse
import getpass
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
CONFIG_PATH = SCRIPT_DIR / "config.json"

DEFAULT_CONFIG = {
    "guild_id": "1238452317838377035",
    "channel_id": "1519697303080796303",
    "audio": (
        "D:\\Stuff\\OpusAudio\\DJ RECAP 2025 VIRAL TIK-TOK TERBARU "
        "FULL PARTY JEDAG JEDUG \u2022 BONGOBARBAR_Horeg.ogg"
    ),
    "selfbot": True,
    "ffmpeg": "ffmpeg",
    "tokens": [],
}


def LoadConfig(PathValue):
    Config = dict(DEFAULT_CONFIG)
    if PathValue.is_file():
        with PathValue.open("r", encoding="utf-8") as Handle:
            Config.update(json.load(Handle))
    return Config


def ResolveFfmpeg(Config, Override):
    Candidates = [Override, os.environ.get("FFMPEG_PATH"), Config.get("ffmpeg"), "ffmpeg"]
    for Candidate in Candidates:
        if not Candidate:
            continue
        PathValue = Path(Candidate).expanduser()
        if PathValue.is_file():
            return str(PathValue)
        Found = shutil.which(str(Candidate))
        if Found:
            return Found
    return None


def ResolveVoicePlay(Override):
    Candidates = []
    if Override:
        Candidates.append(Path(Override).expanduser())
    Environment = os.environ.get("VOICEPLAY_PATH")
    if Environment:
        Candidates.append(Path(Environment).expanduser())
    Candidates.extend(
        [
            SCRIPT_DIR / "VoicePlay.exe",
            SCRIPT_DIR / "VoicePlay",
            SCRIPT_DIR / "build" / "Release" / "VoicePlay.exe",
            SCRIPT_DIR / "build" / "Release" / "VoicePlay",
            SCRIPT_DIR / "build" / "VoicePlay",
        ]
    )
    for Candidate in Candidates:
        if Candidate.is_file():
            if os.name != "nt":
                Candidate.chmod(Candidate.stat().st_mode | 0o111)
            return str(Candidate)
    return None


def ResolveTokens(Arguments, Config):
    Tokens = Arguments.token
    if not Tokens:
        Environment = os.environ.get("VOICE_TOKENS")
        if Environment:
            Tokens = [Item.strip() for Item in Environment.split(",") if Item.strip()]
    if not Tokens:
        Tokens = Config.get("tokens") or []
    Tokens = [str(Token).strip() for Token in Tokens if str(Token).strip()]
    if not Tokens:
        Entered = getpass.getpass("Discord token: ").strip()
        if Entered:
            Tokens = [Entered]
    if not Tokens:
        raise RuntimeError(
            "No tokens configured. Add them to config.json, pass --token, "
            "or set VOICE_TOKENS."
        )
    return Tokens


def Main():
    Parser = argparse.ArgumentParser(
        description="Play a local audio file in a Discord voice channel with VoicePlay."
    )
    Parser.add_argument("source", nargs="?", help="Local audio file path")
    Parser.add_argument("--guild", help="Guild ID override")
    Parser.add_argument("--channel", help="Voice channel ID override")
    Parser.add_argument("--token", action="append", help="Token override; repeat for multiple bots")
    Parser.add_argument("--ffmpeg", help="Path to the ffmpeg executable")
    Parser.add_argument("--voiceplay", help="Path to the VoicePlay executable")
    Parser.add_argument("--config", default=str(CONFIG_PATH), help="Path to config.json")
    Parser.add_argument("--dry-run", action="store_true", help="Print the command without launching VoicePlay")
    Parser.add_argument("--debug", action="store_true", help="Enable VoicePlay debug output")
    Mode = Parser.add_mutually_exclusive_group()
    Mode.add_argument("--selfbot", dest="selfbot", action="store_true", help="Use self-bot mode")
    Mode.add_argument("--bot", dest="selfbot", action="store_false", help="Use normal bot mode")
    Parser.set_defaults(selfbot=None)
    Arguments = Parser.parse_args()

    try:
        Config = LoadConfig(Path(Arguments.config).expanduser())
        Ffmpeg = ResolveFfmpeg(Config, Arguments.ffmpeg)
        if not Ffmpeg:
            raise RuntimeError(
                "ffmpeg is not installed or not on PATH. Install ffmpeg or set FFMPEG_PATH."
            )

        VoicePlay = ResolveVoicePlay(Arguments.voiceplay)
        if not VoicePlay:
            raise RuntimeError(
                "VoicePlay was not found. Build it or set VOICEPLAY_PATH."
            )

        Guild = Arguments.guild or os.environ.get("VOICE_GUILD_ID") or str(Config.get("guild_id") or "")
        Channel = Arguments.channel or os.environ.get("VOICE_CHANNEL_ID") or str(Config.get("channel_id") or "")
        if not Guild or not Channel:
            raise RuntimeError("Guild ID and channel ID are required.")

        SelfBot = Arguments.selfbot
        if SelfBot is None:
            SelfBot = bool(Config.get("selfbot", True))

        Source = Arguments.source
        if not Source:
            Candidate = Config.get("audio")
            if Candidate and Path(str(Candidate)).expanduser().is_file():
                Source = str(Candidate)
            else:
                Source = input("Audio path: ").strip()
        if not Source:
            raise RuntimeError("No audio file provided.")

        AudioPath = Path(Source).expanduser()
        if not AudioPath.is_file():
            raise RuntimeError(f"Audio file not found: {AudioPath}")
        Audio = str(AudioPath.resolve())

        Tokens = ResolveTokens(Arguments, Config)

        Command = [VoicePlay]
        if SelfBot:
            Command.append("--selfbot")
        Command.extend([Guild, Channel, Audio])
        Command.extend(Tokens)

        Display = [VoicePlay]
        if SelfBot:
            Display.append("--selfbot")
        Display.extend([Guild, Channel, Audio])
        Display.extend(["***" for _ in Tokens])
        if os.name == "nt":
            Shown = subprocess.list2cmdline(Display)
        else:
            Shown = shlex.join(Display)
        print(f"Running (tokens hidden): {Shown}")
        print("VoicePlay is encoding/connecting; playback info appears when it starts.")

        if Arguments.dry_run:
            return 0

        Environment = os.environ.copy()
        Environment["FFMPEG_PATH"] = Ffmpeg
        if Arguments.debug:
            Environment["VOICE_DEBUG"] = "1"
        Result = subprocess.run(Command, cwd=str(SCRIPT_DIR), env=Environment)
        if Result.returncode != 0:
            print(f"VoicePlay exited with code {Result.returncode}", file=sys.stderr)
        return Result.returncode
    except Exception as Error:
        print(f"error: {Error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(Main())
