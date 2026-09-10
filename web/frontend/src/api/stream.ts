import type { StreamServerMsg, StreamStart } from './types'

export interface StreamClientOptions {
  token: string
  onMessage: (msg: StreamServerMsg) => void
  onAudio: (seq: number, pcm: Float32Array, sampleRate: number) => void
  onClose: (ev: CloseEvent) => void
  onError: (ev: Event) => void
}

/** Thin wrapper around WS /v1/stream: pairs each `audio` JSON header with the
 * binary float32-LE frame that immediately follows it. */
export class StreamClient {
  private ws: WebSocket | null = null
  private pendingAudioHeader: StreamServerMsg | null = null

  constructor(private opts: StreamClientOptions) {}

  connect(start: StreamStart): Promise<void> {
    return new Promise((resolve, reject) => {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws'
      const url = new URL(`${proto}://${location.host}/v1/stream`)
      if (this.opts.token) url.searchParams.set('access_token', this.opts.token)
      const ws = new WebSocket(url)
      ws.binaryType = 'arraybuffer'
      this.ws = ws

      ws.onopen = () => {
        ws.send(JSON.stringify(start))
        resolve()
      }
      ws.onerror = (ev) => {
        this.opts.onError(ev)
        reject(new Error('websocket error'))
      }
      ws.onclose = (ev) => this.opts.onClose(ev)
      ws.onmessage = (ev: MessageEvent) => this.handleMessage(ev)
    })
  }

  private handleMessage(ev: MessageEvent): void {
    if (typeof ev.data !== 'string') {
      if (this.pendingAudioHeader) {
        const pcm = new Float32Array(ev.data as ArrayBuffer)
        this.opts.onAudio(this.pendingAudioHeader.seq, pcm, this.pendingAudioHeader.sample_rate || 22050)
        this.pendingAudioHeader = null
      }
      return
    }
    const msg = JSON.parse(ev.data) as StreamServerMsg
    if (msg.type === 'audio') {
      this.pendingAudioHeader = msg
    }
    this.opts.onMessage(msg)
  }

  sendAudio(frame: ArrayBuffer): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(frame)
  }

  stop(): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ type: 'stop' }))
    }
  }

  close(): void {
    this.ws?.close()
    this.ws = null
  }
}

/** Reorders audio buffers by `seq` before handing them to a playback sink,
 * defensively — the server already reorders, but frames could still arrive
 * out of order across proxies/browsers. Skips forward past irrecoverable
 * gaps once `done` is seen. */
export class AudioReorder {
  private pending = new Map<number, { pcm: Float32Array; sampleRate: number }>()
  private nextSeq = 0

  constructor(private sink: (pcm: Float32Array, sampleRate: number) => void) {}

  push(seq: number, pcm: Float32Array, sampleRate: number): void {
    this.pending.set(seq, { pcm, sampleRate })
    this.drain()
  }

  private drain(): void {
    while (this.pending.has(this.nextSeq)) {
      const item = this.pending.get(this.nextSeq)!
      this.pending.delete(this.nextSeq)
      this.sink(item.pcm, item.sampleRate)
      this.nextSeq++
    }
  }

  /** Call on `done`/error recovery: emit whatever is left, in seq order, skipping gaps. */
  flush(): void {
    const keys = [...this.pending.keys()].sort((a, b) => a - b)
    for (const k of keys) {
      const item = this.pending.get(k)!
      this.sink(item.pcm, item.sampleRate)
    }
    this.pending.clear()
  }

  reset(): void {
    this.pending.clear()
    this.nextSeq = 0
  }
}
