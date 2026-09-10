import workletUrl from './pcm-worklet.js?url'

export interface MicCapture {
  stop(): void
}

export interface MicCaptureOptions {
  targetRate?: number
  onFrame: (frame: ArrayBuffer) => void
}

/** Opens the mic, taps 16kHz-resampled float32 PCM frames via an AudioWorklet. */
export async function startMicCapture(opts: MicCaptureOptions): Promise<MicCapture> {
  const targetRate = opts.targetRate ?? 16000

  const stream = await navigator.mediaDevices.getUserMedia({
    audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
  })

  let ctx: AudioContext
  try {
    ctx = new AudioContext({ sampleRate: targetRate })
  } catch {
    ctx = new AudioContext()
  }

  await ctx.audioWorklet.addModule(workletUrl)
  const source = ctx.createMediaStreamSource(stream)
  const node = new AudioWorkletNode(ctx, 'pcm-tap', {
    processorOptions: { targetRate },
  })
  node.port.onmessage = (e: MessageEvent<ArrayBuffer>) => opts.onFrame(e.data)
  source.connect(node)
  // AudioWorkletNode must be connected to a destination for `process` to run
  // in some browsers, even though we discard the (silent) node output.
  node.connect(ctx.destination)

  return {
    stop() {
      node.disconnect()
      source.disconnect()
      stream.getTracks().forEach((t) => t.stop())
      void ctx.close()
    },
  }
}
