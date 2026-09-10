<script setup lang="ts">
import { onUnmounted, ref } from 'vue'

const status = ref<'idle' | 'listening' | 'error'>('idle')
const errorMsg = ref('')
const volume = ref(0.7)

let ctx: AudioContext | null = null
let micStream: MediaStream | null = null
let source: MediaStreamAudioSourceNode | null = null
let gain: GainNode | null = null

async function start(): Promise<void> {
  errorMsg.value = ''
  try {
    // echoCancellation off: this IS the loopback, cancelling it would mostly
    // silence what you're testing. Use headphones to avoid a feedback howl.
    micStream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    })
    ctx = new AudioContext()
    source = ctx.createMediaStreamSource(micStream)
    gain = ctx.createGain()
    gain.gain.value = volume.value
    source.connect(gain)
    gain.connect(ctx.destination)
    status.value = 'listening'
  } catch (e) {
    errorMsg.value = e instanceof Error ? e.message : String(e)
    status.value = 'error'
  }
}

function stop(): void {
  source?.disconnect()
  gain?.disconnect()
  micStream?.getTracks().forEach((t) => t.stop())
  void ctx?.close()
  source = gain = micStream = ctx = null
  status.value = 'idle'
}

function toggle(): void {
  if (status.value === 'listening') stop()
  else void start()
}

function onVolume(): void {
  if (gain) gain.gain.value = volume.value
}

onUnmounted(stop)
</script>

<template>
  <section>
    <p class="hint">
      Pure mic → speaker passthrough — no translation, no network. Use this to check your
      microphone and speakers work before trying a live session. <strong>Use headphones</strong> —
      without them the speaker output will feed back into the mic and howl.
    </p>

    <div class="controls">
      <button class="primary" @click="toggle">{{ status === 'listening' ? 'Stop' : 'Start' }}</button>
      <span class="status">{{ status }}</span>
    </div>

    <label class="volume">
      Volume
      <input type="range" min="0" max="1" step="0.01" v-model.number="volume" @input="onVolume" />
    </label>

    <p v-if="errorMsg" class="err">{{ errorMsg }}</p>
  </section>
</template>

<style scoped>
.hint {
  font-size: 0.85rem;
  color: var(--muted);
  max-width: 40rem;
  margin-bottom: 1rem;
}
.controls {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 1rem;
}
button.primary {
  background: var(--accent);
  color: var(--accent-fg);
  border: 1px solid var(--accent);
  border-radius: 8px;
  padding: 0.5rem 1rem;
  cursor: pointer;
}
.status {
  font-size: 0.85rem;
  color: var(--muted);
}
.volume {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  font-size: 0.85rem;
  color: var(--muted);
}
.err {
  color: var(--err);
  margin-top: 0.75rem;
}
</style>
