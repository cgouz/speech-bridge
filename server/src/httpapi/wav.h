// wav — RIFF/WAVE decode + PCM16 encode. Direct port of
// app/internal/httpapi/wav.go.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace sb::httpapi {

class NotWavError : public std::runtime_error {
 public:
  NotWavError() : std::runtime_error("not a WAV file (bad RIFF/WAVE header)") {}
};

class WavError : public std::runtime_error {
 public:
  explicit WavError(const std::string &msg) : std::runtime_error(msg) {}
};

struct WavAudio {
  std::vector<float> samples;  // mono
  int sample_rate = 0;
  int channels = 0;
  double duration_s = 0;
};

// Parses a PCM (int16/uint8) or IEEE-float (float32) WAV, downmixing to mono.
// Validated by header, never by extension. Throws NotWavError / WavError.
WavAudio DecodeWav(const std::string &data);

// Writes mono float32 PCM as a 16-bit PCM WAV.
std::string EncodeWav(const std::vector<float> &samples, int sampleRate);

}  // namespace sb::httpapi
