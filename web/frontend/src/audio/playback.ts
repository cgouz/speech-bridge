/** Schedules float32 PCM buffers back-to-back on one AudioContext for gapless playback. */
export class PlaybackQueue {
  private ctx: AudioContext | null = null
  private playHead = 0

  play(pcm: Float32Array, sampleRate: number): void {
    if (!this.ctx) {
      this.ctx = new AudioContext()
      this.playHead = this.ctx.currentTime
    }
    const ctx = this.ctx
    const buf = ctx.createBuffer(1, pcm.length, sampleRate || 22050)
    // lib.dom's copyToChannel wants Float32Array<ArrayBuffer> specifically;
    // callers only guarantee the looser ArrayBufferLike-backed Float32Array.
    buf.copyToChannel(pcm as Float32Array<ArrayBuffer>, 0)
    const src = ctx.createBufferSource()
    src.buffer = buf
    src.connect(ctx.destination)
    const t = Math.max(ctx.currentTime, this.playHead)
    src.start(t)
    this.playHead = t + buf.duration
  }

  close(): void {
    if (this.ctx) {
      void this.ctx.close()
      this.ctx = null
    }
    this.playHead = 0
  }
}
