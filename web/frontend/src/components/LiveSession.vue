<script setup lang="ts">
import { onUnmounted, reactive, ref } from 'vue'
import { useSettingsStore } from '../stores/settings'
import { StreamClient, AudioReorder } from '../api/stream'
import { startMicCapture, type MicCapture } from '../audio/capture'
import { PlaybackQueue } from '../audio/playback'
import type { StreamServerMsg } from '../api/types'

interface Line {
  seq: number
  source: string
  translation: string
  t0?: number
  t1?: number
  error?: { code?: string; stage?: string; text?: string }
}

type Status = 'idle' | 'connecting' | 'listening' | 'stopping' | 'closed' | 'error'

const settings = useSettingsStore()
const status = ref<Status>('idle')
const partial = ref('')
const lines = reactive<Line[]>([])
const sessionError = ref('')

let client: StreamClient | null = null
let mic: MicCapture | null = null
let playback: PlaybackQueue | null = null
let reorder: AudioReorder | null = null

function lineFor(seq: number): Line {
  let line = lines.find((l) => l.seq === seq)
  if (!line) {
    line = { seq, source: '', translation: '' }
    lines.push(line)
  }
  return line
}

function handleMessage(msg: StreamServerMsg): void {
  switch (msg.type) {
    case 'partial':
      partial.value = msg.text ?? ''
      break
    case 'transcript': {
      partial.value = ''
      const line = lineFor(msg.seq)
      line.source = msg.text ?? ''
      line.t0 = msg.t0_ms
      line.t1 = msg.t1_ms
      break
    }
    case 'translation':
      lineFor(msg.seq).translation = msg.text ?? ''
      break
    case 'error':
      lineFor(msg.seq).error = { code: msg.code, stage: msg.stage, text: msg.text }
      break
    case 'done':
      reorder?.flush()
      teardownMic()
      client?.close()
      status.value = 'closed'
      break
  }
}

async function start(): Promise<void> {
  sessionError.value = ''
  lines.length = 0
  partial.value = ''
  status.value = 'connecting'

  playback = new PlaybackQueue()
  reorder = new AudioReorder((pcm, sr) => playback?.play(pcm, sr))
  client = new StreamClient({
    token: settings.token,
    onMessage: handleMessage,
    onAudio: (seq, pcm, sr) => reorder?.push(seq, pcm, sr),
    onClose: () => {
      if (status.value !== 'closed') status.value = 'closed'
      teardownMic()
    },
    onError: () => {
      sessionError.value = 'WebSocket connection error'
      status.value = 'error'
    },
  })

  try {
    await client.connect({
      type: 'start',
      source_lang: settings.sourceLang,
      target_lang: settings.targetLang,
      voice: settings.voice,
      sample_rate: 16000,
      format: 'f32',
    })
  } catch {
    status.value = 'error'
    return
  }

  try {
    mic = await startMicCapture({ targetRate: 16000, onFrame: (buf) => client?.sendAudio(buf) })
  } catch (e) {
    sessionError.value = e instanceof Error ? `Microphone error: ${e.message}` : 'Microphone access denied'
    status.value = 'error'
    client.close()
    client = null
    return
  }

  status.value = 'listening'
}

function stop(): void {
  if (status.value !== 'listening') return
  status.value = 'stopping'
  teardownMic()
  client?.stop()
}

function teardownMic(): void {
  mic?.stop()
  mic = null
}

function toggle(): void {
  if (status.value === 'listening') stop()
  else if (status.value === 'idle' || status.value === 'closed' || status.value === 'error') void start()
}

onUnmounted(() => {
  teardownMic()
  client?.close()
  playback?.close()
})
</script>

<template>
  <section>
    <div class="controls">
      <button class="primary" :disabled="status === 'connecting' || status === 'stopping'" @click="toggle">
        {{ status === 'listening' ? 'Stop' : 'Start speaking' }}
      </button>
      <span class="status">{{ status }}</span>
    </div>
    <p v-if="sessionError" class="err">{{ sessionError }}</p>

    <div class="caption">
      <div class="lbl">Live</div>
      <div class="partial">{{ partial }}</div>
    </div>

    <div class="transcript">
      <div v-for="line in lines" :key="line.seq" class="pair">
        <div class="src">{{ line.source }}</div>
        <div class="dst">{{ line.translation || (line.source ? '…' : '') }}</div>
        <div v-if="line.error" class="badge warn">
          {{ line.error.stage }}/{{ line.error.code }}<span v-if="line.error.text"> — {{ line.error.text }}</span>
        </div>
      </div>
      <p v-if="lines.length === 0" class="muted">Press "Start speaking" and grant microphone access.</p>
    </div>
  </section>
</template>

<style scoped>
.controls {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 0.75rem;
}
button.primary {
  background: var(--accent);
  color: var(--accent-fg);
  border: 1px solid var(--accent);
  border-radius: 8px;
  padding: 0.5rem 1rem;
  cursor: pointer;
}
button.primary[disabled] {
  opacity: 0.5;
  cursor: default;
}
.status {
  font-size: 0.85rem;
  color: var(--muted);
}
.err {
  color: var(--err);
}
.muted {
  color: var(--muted);
  font-size: 0.85rem;
}
.caption {
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 0.75rem;
  margin-bottom: 0.75rem;
  min-height: 1.5rem;
}
.lbl {
  font-size: 0.7rem;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: var(--muted);
  margin-bottom: 0.25rem;
}
.partial {
  font-style: italic;
  opacity: 0.7;
}
.transcript {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  max-height: 24rem;
  overflow-y: auto;
}
.pair {
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 0.6rem 0.75rem;
}
.src {
  font-size: 0.95rem;
}
.dst {
  font-size: 0.95rem;
  color: var(--accent);
  margin-top: 0.15rem;
}
</style>
