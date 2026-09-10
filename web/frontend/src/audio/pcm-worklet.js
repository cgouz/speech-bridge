// AudioWorkletProcessor that taps the mic input, linearly resamples it to
// `targetRate` if the AudioContext's actual rate differs (some browsers
// ignore the requested `sampleRate` on construction), and posts fixed-size
// float32 chunks (~250ms) back to the main thread for WS transport.
//
// Plain JS, not TS: it runs in the AudioWorkletGlobalScope, where
// `sampleRate`/`registerProcessor` are host globals, not imports — and it's
// loaded via a `?url` import (see capture.ts), which must not resolve to a
// `.ts` file (Vite/rollup's asset pipeline treats `.ts` as the MPEG
// transport-stream MIME type and inlines it as a `data:video/mp2t` URL,
// which the browser then refuses to load as a worklet module).
class PCMTap extends AudioWorkletProcessor {
  constructor(options) {
    super()
    const opts = (options && options.processorOptions) || {}
    const targetRate = opts.targetRate || 16000
    this.ratio = sampleRate / targetRate // input samples per output sample
    this.chunkSamples = Math.round(targetRate * 0.25) // ~250ms
    this.inBuf = []
    this.outBuf = []
    this.readPos = 0 // fractional read cursor into inBuf, in input-sample units
  }

  process(inputs) {
    const ch = inputs[0] && inputs[0][0]
    if (!ch || ch.length === 0) return true
    for (let i = 0; i < ch.length; i++) this.inBuf.push(ch[i])

    if (this.ratio === 1) {
      for (let i = 0; i < this.inBuf.length; i++) this.outBuf.push(this.inBuf[i])
      this.inBuf.length = 0
    } else {
      while (true) {
        const i0 = Math.floor(this.readPos)
        const i1 = i0 + 1
        if (i1 >= this.inBuf.length) break
        const frac = this.readPos - i0
        this.outBuf.push(this.inBuf[i0] * (1 - frac) + this.inBuf[i1] * frac)
        this.readPos += this.ratio
      }
      const drop = Math.floor(this.readPos)
      if (drop > 0) {
        this.inBuf.splice(0, drop)
        this.readPos -= drop
      }
    }

    while (this.outBuf.length >= this.chunkSamples) {
      const out = new Float32Array(this.outBuf.splice(0, this.chunkSamples))
      this.port.postMessage(out.buffer, [out.buffer])
    }
    return true
  }
}

registerProcessor('pcm-tap', PCMTap)
