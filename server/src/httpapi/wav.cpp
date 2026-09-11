#include "wav.h"

#include <cstdint>
#include <cstring>

namespace sb::httpapi {

namespace {

uint16_t ReadU16LE(const std::string &b, size_t off) {
  return static_cast<uint16_t>(static_cast<uint8_t>(b[off])) |
         (static_cast<uint16_t>(static_cast<uint8_t>(b[off + 1])) << 8);
}

uint32_t ReadU32LE(const std::string &b, size_t off) {
  return static_cast<uint32_t>(static_cast<uint8_t>(b[off])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b[off + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b[off + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b[off + 3])) << 24);
}

void WriteU16LE(std::string &b, uint16_t v) {
  b.push_back(static_cast<char>(v & 0xFF));
  b.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void WriteU32LE(std::string &b, uint32_t v) {
  b.push_back(static_cast<char>(v & 0xFF));
  b.push_back(static_cast<char>((v >> 8) & 0xFF));
  b.push_back(static_cast<char>((v >> 16) & 0xFF));
  b.push_back(static_cast<char>((v >> 24) & 0xFF));
}

}  // namespace

WavAudio DecodeWav(const std::string &b) {
  if (b.size() < 44 || b.compare(0, 4, "RIFF") != 0 || b.compare(8, 4, "WAVE") != 0) {
    throw NotWavError();
  }

  uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0;
  bool haveFmt = false;
  size_t dataOff = 0, dataLen = 0;
  bool haveData = false;

  size_t pos = 12;
  while (pos + 8 <= b.size()) {
    std::string id = b.substr(pos, 4);
    int64_t size = static_cast<int64_t>(ReadU32LE(b, pos + 4));
    pos += 8;
    if (size < 0 || pos + static_cast<size_t>(size) > b.size()) {
      size = static_cast<int64_t>(b.size() - pos);  // tolerate a truncated/streamed data chunk
    }
    if (id == "fmt ") {
      if (static_cast<size_t>(size) < 16) throw WavError("wav: short fmt chunk");
      audioFormat = ReadU16LE(b, pos);
      numChannels = ReadU16LE(b, pos + 2);
      sampleRate = ReadU32LE(b, pos + 4);
      bitsPerSample = ReadU16LE(b, pos + 14);
      if (audioFormat == 0xFFFE && static_cast<size_t>(size) >= 26) {  // WAVE_FORMAT_EXTENSIBLE
        audioFormat = ReadU16LE(b, pos + 24);
      }
      haveFmt = true;
    } else if (id == "data") {
      dataOff = pos;
      dataLen = static_cast<size_t>(size);
      haveData = true;
    }
    pos += static_cast<size_t>(size);
    if (size % 2 == 1) pos++;  // chunks are word-aligned
  }

  if (!haveFmt || !haveData) throw WavError("wav: missing fmt or data chunk");
  if (numChannels == 0) throw WavError("wav: zero channels");

  std::vector<float> interleaved;
  if (audioFormat == 1 && bitsPerSample == 16) {
    size_t n = dataLen / 2;
    interleaved.resize(n);
    for (size_t i = 0; i < n; i++) {
      int16_t s = static_cast<int16_t>(ReadU16LE(b, dataOff + i * 2));
      interleaved[i] = static_cast<float>(s) / 32768.0f;
    }
  } else if (audioFormat == 1 && bitsPerSample == 8) {
    interleaved.resize(dataLen);
    for (size_t i = 0; i < dataLen; i++) {
      uint8_t u = static_cast<uint8_t>(b[dataOff + i]);
      interleaved[i] = (static_cast<float>(u) - 128) / 128.0f;
    }
  } else if (audioFormat == 3 && bitsPerSample == 32) {
    size_t n = dataLen / 4;
    interleaved.resize(n);
    for (size_t i = 0; i < n; i++) {
      uint32_t bits = ReadU32LE(b, dataOff + i * 4);
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      interleaved[i] = f;
    }
  } else {
    throw WavError("wav: unsupported format=" + std::to_string(audioFormat) +
                    " bits=" + std::to_string(bitsPerSample) + " (need PCM16/PCM8 or float32)");
  }

  int ch = numChannels;
  std::vector<float> mono;
  if (ch > 1) {
    size_t frames = interleaved.size() / ch;
    mono.resize(frames);
    for (size_t i = 0; i < frames; i++) {
      float sum = 0;
      for (int c = 0; c < ch; c++) sum += interleaved[i * ch + c];
      mono[i] = sum / static_cast<float>(ch);
    }
  } else {
    mono = std::move(interleaved);
  }

  WavAudio out;
  out.samples = std::move(mono);
  out.sample_rate = static_cast<int>(sampleRate);
  out.channels = ch;
  out.duration_s = sampleRate ? static_cast<double>(out.samples.size()) / static_cast<double>(sampleRate) : 0;
  return out;
}

std::string EncodeWav(const std::vector<float> &samples, int sampleRate) {
  constexpr int bits = 16;
  uint32_t dataLen = static_cast<uint32_t>(samples.size() * 2);
  std::string out;
  out.reserve(44 + dataLen);
  out += "RIFF";
  WriteU32LE(out, 36 + dataLen);
  out += "WAVE";
  out += "fmt ";
  WriteU32LE(out, 16);
  WriteU16LE(out, 1);  // PCM
  WriteU16LE(out, 1);  // mono
  WriteU32LE(out, static_cast<uint32_t>(sampleRate));
  WriteU32LE(out, static_cast<uint32_t>(sampleRate * bits / 8));
  WriteU16LE(out, bits / 8);
  WriteU16LE(out, bits);
  out += "data";
  WriteU32LE(out, dataLen);
  for (float s : samples) {
    int32_t v = static_cast<int32_t>(s * 32767.0f);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    WriteU16LE(out, static_cast<uint16_t>(static_cast<int16_t>(v)));
  }
  return out;
}

}  // namespace sb::httpapi
