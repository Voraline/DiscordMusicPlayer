use std::io::Read;
use std::path::Path;
use std::process::{Command, Stdio};

use audiopus::coder::Encoder;
use audiopus::{Application, Bitrate, Channels, SampleRate, Signal};

use crate::types::{EncodedAudio, EncoderOptions};

const SAMPLE_RATE: u32 = 48_000;
const CHANNELS: u8 = 2;
const FRAME_SAMPLES: usize = 960;
const FRAME_BYTES: usize = FRAME_SAMPLES * CHANNELS as usize * 2;

pub fn encode_file(path: &Path, options: &EncoderOptions) -> Result<EncodedAudio, String> {
    let mut command = Command::new(&options.ffmpeg_path);
    command
        .arg("-v")
        .arg("error")
        .arg("-nostdin")
        .arg("-i")
        .arg(path)
        .arg("-vn")
        .arg("-sn")
        .arg("-dn")
        .arg("-f")
        .arg("s16le")
        .arg("-ac")
        .arg("2")
        .arg("-ar")
        .arg("48000")
        .arg("-")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());

    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x0800_0000);
    }

    let mut child = command
        .spawn()
        .map_err(|error| format!("Failed to start FFmpeg: {error}"))?;
    let mut stdout = child
        .stdout
        .take()
        .ok_or_else(|| "Failed to capture FFmpeg output".to_string())?;

    let mut encoder = Encoder::new(SampleRate::Hz48000, Channels::Stereo, Application::Audio)
        .map_err(|error| format!("Failed to create Opus encoder: {error}"))?;
    encoder
        .set_bitrate(Bitrate::BitsPerSecond(options.bitrate as i32))
        .map_err(|error| format!("Failed to set Opus bitrate: {error}"))?;
    encoder
        .set_complexity(options.complexity)
        .map_err(|error| format!("Failed to set Opus complexity: {error}"))?;
    encoder
        .set_signal(Signal::Music)
        .map_err(|error| format!("Failed to set Opus signal: {error}"))?;

    let mut frames: Vec<Vec<u8>> = Vec::new();
    let mut pending: Vec<u8> = Vec::with_capacity(32_768);
    let mut buffer = [0u8; 16_384];
    let mut samples = [0i16; FRAME_SAMPLES * CHANNELS as usize];
    let mut packet = [0u8; 4000];

    loop {
        let read = match stdout.read(&mut buffer) {
            Ok(0) => break,
            Ok(read) => read,
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(format!("Failed to read FFmpeg output: {error}")),
        };
        pending.extend_from_slice(&buffer[..read]);

        let mut offset = 0;
        while pending.len() - offset >= FRAME_BYTES {
            let (chunks, _) = pending[offset..offset + FRAME_BYTES].as_chunks::<2>();
            for (sample, bytes) in samples.iter_mut().zip(chunks) {
                *sample = i16::from_le_bytes(*bytes);
            }
            let encoded = encoder
                .encode(&samples, &mut packet)
                .map_err(|error| format!("Opus encoding failed: {error}"))?;
            frames.push(packet[..encoded].to_vec());
            offset += FRAME_BYTES;
        }

        if offset > 0 {
            pending.drain(..offset);
        }
    }

    let status = child
        .wait()
        .map_err(|error| format!("Failed to wait for FFmpeg: {error}"))?;

    if frames.is_empty() {
        return Err(format!(
            "No audio decoded from {} (FFmpeg exit {})",
            path.display(),
            status
                .code()
                .map(|code| code.to_string())
                .unwrap_or_else(|| "signal".to_string())
        ));
    }

    Ok(EncodedAudio {
        frames,
        sample_rate: SAMPLE_RATE,
        channels: CHANNELS,
        frame_duration_ms: 20,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encodes_generated_tone() {
        let Ok(ffmpeg) = std::env::var("FFMPEG_PATH") else {
            return;
        };

        let directory = std::env::temp_dir().join("voice-rust-tests");
        std::fs::create_dir_all(&directory).unwrap();
        let source = directory.join("tone.wav");
        let status = Command::new(&ffmpeg)
            .args([
                "-y",
                "-v",
                "error",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=440:duration=1",
                "-ac",
                "2",
                "-ar",
                "48000",
            ])
            .arg(&source)
            .status()
            .unwrap();
        assert!(status.success());

        let options = EncoderOptions {
            ffmpeg_path: ffmpeg,
            ..EncoderOptions::default()
        };
        let audio = encode_file(&source, &options).unwrap();
        assert_eq!(audio.frames.len(), 50);
        assert!(audio.frames.iter().all(|frame| !frame.is_empty()));
        let _ = std::fs::remove_file(source);
    }
}
