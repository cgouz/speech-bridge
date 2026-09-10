package pipeline

// ResampleLinear resamples mono float32 PCM from one rate to another with
// linear interpolation. Good enough for 16 kHz speech front-ends; STT quality
// is dominated by the model, not the resampler.
func ResampleLinear(in []float32, from, to int) []float32 {
	if from == to || len(in) == 0 || from <= 0 || to <= 0 {
		return in
	}
	ratio := float64(from) / float64(to)
	outLen := int(float64(len(in)) / ratio)
	if outLen <= 0 {
		return nil
	}
	out := make([]float32, outLen)
	for i := range out {
		src := float64(i) * ratio
		j := int(src)
		frac := float32(src - float64(j))
		if j+1 < len(in) {
			out[i] = in[j]*(1-frac) + in[j+1]*frac
		} else {
			out[i] = in[len(in)-1]
		}
	}
	return out
}

// DownmixToMono averages interleaved channels into a single channel.
func DownmixToMono(interleaved []float32, channels int) []float32 {
	if channels <= 1 {
		return interleaved
	}
	n := len(interleaved) / channels
	out := make([]float32, n)
	for i := 0; i < n; i++ {
		var sum float32
		for c := 0; c < channels; c++ {
			sum += interleaved[i*channels+c]
		}
		out[i] = sum / float32(channels)
	}
	return out
}

const sttSampleRate = 16000

// chunkSamples is ~250 ms at 16 kHz — the frame size the STT stream is fed.
const chunkSamples = sttSampleRate / 4
