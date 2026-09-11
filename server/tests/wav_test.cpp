#include <catch_amalgamated.hpp>

#include "../src/httpapi/wav.h"

using namespace sb::httpapi;

TEST_CASE("WAV encode/decode round-trip", "[wav]") {
  std::vector<float> samples = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f};
  std::string wav = EncodeWav(samples, 16000);

  WavAudio decoded = DecodeWav(wav);
  REQUIRE(decoded.sample_rate == 16000);
  REQUIRE(decoded.channels == 1);
  REQUIRE(decoded.samples.size() == samples.size());
  for (size_t i = 0; i < samples.size(); i++) {
    CHECK(decoded.samples[i] == Catch::Approx(samples[i]).margin(0.001));
  }
}

TEST_CASE("DecodeWav rejects non-WAV data", "[wav]") {
  CHECK_THROWS_AS(DecodeWav("this is not a wav file at all"), NotWavError);
}

TEST_CASE("DecodeWav computes duration", "[wav]") {
  std::vector<float> samples(16000, 0.0f);  // 1 second at 16kHz
  std::string wav = EncodeWav(samples, 16000);
  WavAudio decoded = DecodeWav(wav);
  CHECK(decoded.duration_s == Catch::Approx(1.0).margin(0.01));
}
