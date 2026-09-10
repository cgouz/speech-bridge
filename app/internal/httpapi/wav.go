package httpapi

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
)

// errNotWAV is returned when a payload is not a RIFF/WAVE file by header.
var errNotWAV = errors.New("not a WAV file (bad RIFF/WAVE header)")

type wavAudio struct {
	Samples    []float32 // mono
	SampleRate int
	Channels   int
	DurationS  float64
}

// decodeWAV parses a PCM (int16) or IEEE-float (float32) WAV from b, downmixing
// to mono. Files are validated by header, never by extension.
func decodeWAV(b []byte) (*wavAudio, error) {
	if len(b) < 44 || string(b[0:4]) != "RIFF" || string(b[8:12]) != "WAVE" {
		return nil, errNotWAV
	}

	var (
		audioFormat   uint16
		numChannels   uint16
		sampleRate    uint32
		bitsPerSample uint16
		data          []byte
		haveFmt       bool
	)

	pos := 12
	for pos+8 <= len(b) {
		id := string(b[pos : pos+4])
		size := int(binary.LittleEndian.Uint32(b[pos+4 : pos+8]))
		pos += 8
		if size < 0 || pos+size > len(b) {
			size = len(b) - pos // tolerate a truncated/streamed data chunk
		}
		chunk := b[pos : pos+size]
		switch id {
		case "fmt ":
			if len(chunk) < 16 {
				return nil, fmt.Errorf("wav: short fmt chunk")
			}
			audioFormat = binary.LittleEndian.Uint16(chunk[0:2])
			numChannels = binary.LittleEndian.Uint16(chunk[2:4])
			sampleRate = binary.LittleEndian.Uint32(chunk[4:8])
			bitsPerSample = binary.LittleEndian.Uint16(chunk[14:16])
			if audioFormat == 0xFFFE && len(chunk) >= 26 { // WAVE_FORMAT_EXTENSIBLE
				audioFormat = binary.LittleEndian.Uint16(chunk[24:26])
			}
			haveFmt = true
		case "data":
			data = chunk
		}
		pos += size
		if size%2 == 1 {
			pos++ // chunks are word-aligned
		}
	}

	if !haveFmt || data == nil {
		return nil, fmt.Errorf("wav: missing fmt or data chunk")
	}
	if numChannels == 0 {
		return nil, fmt.Errorf("wav: zero channels")
	}

	var interleaved []float32
	switch {
	case audioFormat == 1 && bitsPerSample == 16:
		n := len(data) / 2
		interleaved = make([]float32, n)
		for i := 0; i < n; i++ {
			s := int16(binary.LittleEndian.Uint16(data[i*2:]))
			interleaved[i] = float32(s) / 32768.0
		}
	case audioFormat == 1 && bitsPerSample == 8:
		interleaved = make([]float32, len(data))
		for i, u := range data {
			interleaved[i] = (float32(u) - 128) / 128.0
		}
	case audioFormat == 3 && bitsPerSample == 32:
		n := len(data) / 4
		interleaved = make([]float32, n)
		for i := 0; i < n; i++ {
			bits := binary.LittleEndian.Uint32(data[i*4:])
			interleaved[i] = math.Float32frombits(bits)
		}
	default:
		return nil, fmt.Errorf("wav: unsupported format=%d bits=%d (need PCM16/PCM8 or float32)", audioFormat, bitsPerSample)
	}

	ch := int(numChannels)
	mono := interleaved
	if ch > 1 {
		frames := len(interleaved) / ch
		mono = make([]float32, frames)
		for i := 0; i < frames; i++ {
			var sum float32
			for c := 0; c < ch; c++ {
				sum += interleaved[i*ch+c]
			}
			mono[i] = sum / float32(ch)
		}
	}

	return &wavAudio{
		Samples:    mono,
		SampleRate: int(sampleRate),
		Channels:   ch,
		DurationS:  float64(len(mono)) / float64(sampleRate),
	}, nil
}

// encodeWAV writes mono float32 PCM as a 16-bit PCM WAV.
func encodeWAV(w io.Writer, samples []float32, sampleRate int) error {
	const bits = 16
	dataLen := len(samples) * 2
	var hdr [44]byte
	copy(hdr[0:4], "RIFF")
	binary.LittleEndian.PutUint32(hdr[4:8], uint32(36+dataLen))
	copy(hdr[8:12], "WAVE")
	copy(hdr[12:16], "fmt ")
	binary.LittleEndian.PutUint32(hdr[16:20], 16)
	binary.LittleEndian.PutUint16(hdr[20:22], 1) // PCM
	binary.LittleEndian.PutUint16(hdr[22:24], 1) // mono
	binary.LittleEndian.PutUint32(hdr[24:28], uint32(sampleRate))
	binary.LittleEndian.PutUint32(hdr[28:32], uint32(sampleRate*bits/8))
	binary.LittleEndian.PutUint16(hdr[32:34], uint16(bits/8))
	binary.LittleEndian.PutUint16(hdr[34:36], bits)
	copy(hdr[36:40], "data")
	binary.LittleEndian.PutUint32(hdr[40:44], uint32(dataLen))
	if _, err := w.Write(hdr[:]); err != nil {
		return err
	}
	buf := make([]byte, 2)
	for _, s := range samples {
		v := int32(s * 32767)
		if v > 32767 {
			v = 32767
		} else if v < -32768 {
			v = -32768
		}
		binary.LittleEndian.PutUint16(buf, uint16(int16(v)))
		if _, err := w.Write(buf); err != nil {
			return err
		}
	}
	return nil
}
