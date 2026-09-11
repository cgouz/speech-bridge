#include "pipeline.h"

namespace sb::pipeline {

std::vector<float> ResampleLinear(const std::vector<float> &in, int from, int to) {
  if (from == to || in.empty() || from <= 0 || to <= 0) return in;
  double ratio = static_cast<double>(from) / static_cast<double>(to);
  int outLen = static_cast<int>(static_cast<double>(in.size()) / ratio);
  if (outLen <= 0) return {};
  std::vector<float> out(outLen);
  for (int i = 0; i < outLen; i++) {
    double src = static_cast<double>(i) * ratio;
    size_t j = static_cast<size_t>(src);
    float frac = static_cast<float>(src - static_cast<double>(j));
    if (j + 1 < in.size()) {
      out[i] = in[j] * (1 - frac) + in[j + 1] * frac;
    } else {
      out[i] = in.back();
    }
  }
  return out;
}

std::vector<float> DownmixToMono(const std::vector<float> &interleaved, int channels) {
  if (channels <= 1) return interleaved;
  size_t n = interleaved.size() / channels;
  std::vector<float> out(n);
  for (size_t i = 0; i < n; i++) {
    float sum = 0;
    for (int c = 0; c < channels; c++) sum += interleaved[i * channels + c];
    out[i] = sum / static_cast<float>(channels);
  }
  return out;
}

}  // namespace sb::pipeline
