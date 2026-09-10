<script setup lang="ts">
import { ref } from 'vue'
import { useSettingsStore } from '../stores/settings'
import { speak, speechToSpeech, transcribe, translate } from '../api/client'
import { ApiError } from '../api/types'
import type { SpeechToSpeechResponse, TranscribeResponse } from '../api/types'

const settings = useSettingsStore()

const file = ref<File | null>(null)
const busy = ref(false)
const error = ref('')
const s2sResult = ref<SpeechToSpeechResponse | null>(null)
const transcribeResult = ref<TranscribeResponse | null>(null)
const s2sAudioUrl = ref('')

const translateText = ref('')
const translateResult = ref('')

const speakText = ref('')
const speakAudioUrl = ref('')

function onFileChange(e: Event): void {
  const input = e.target as HTMLInputElement
  file.value = input.files?.[0] ?? null
}

function base64ToUrl(b64: string, mime: string): string {
  const bytes = atob(b64)
  const arr = new Uint8Array(bytes.length)
  for (let i = 0; i < bytes.length; i++) arr[i] = bytes.charCodeAt(i)
  return URL.createObjectURL(new Blob([arr], { type: mime }))
}

function describeError(e: unknown): string {
  if (e instanceof ApiError) return `${e.code}${e.stage ? `/${e.stage}` : ''}: ${e.message}`
  return e instanceof Error ? e.message : String(e)
}

async function runSpeechToSpeech(): Promise<void> {
  if (!file.value) return
  busy.value = true
  error.value = ''
  s2sResult.value = null
  if (s2sAudioUrl.value) URL.revokeObjectURL(s2sAudioUrl.value)
  s2sAudioUrl.value = ''
  try {
    const res = await speechToSpeech({
      file: file.value,
      sourceLang: settings.sourceLang,
      targetLang: settings.targetLang,
      voice: settings.voice,
      token: settings.token,
    })
    s2sResult.value = res
    if (res.audio) s2sAudioUrl.value = base64ToUrl(res.audio, 'audio/wav')
  } catch (e) {
    error.value = describeError(e)
  } finally {
    busy.value = false
  }
}

async function runTranscribe(): Promise<void> {
  if (!file.value) return
  busy.value = true
  error.value = ''
  transcribeResult.value = null
  try {
    transcribeResult.value = await transcribe(file.value, settings.sourceLang, settings.token)
  } catch (e) {
    error.value = describeError(e)
  } finally {
    busy.value = false
  }
}

async function runTranslate(): Promise<void> {
  if (!translateText.value.trim()) return
  busy.value = true
  error.value = ''
  try {
    const res = await translate(translateText.value, settings.sourceLang, settings.targetLang, settings.token)
    translateResult.value = res.translation
  } catch (e) {
    error.value = describeError(e)
  } finally {
    busy.value = false
  }
}

async function runSpeak(): Promise<void> {
  if (!speakText.value.trim()) return
  busy.value = true
  error.value = ''
  if (speakAudioUrl.value) URL.revokeObjectURL(speakAudioUrl.value)
  speakAudioUrl.value = ''
  try {
    const res = await speak(speakText.value, settings.targetLang, settings.voice, settings.token)
    speakAudioUrl.value = base64ToUrl(res.audio, 'audio/wav')
  } catch (e) {
    error.value = describeError(e)
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <section class="stack">
    <p v-if="error" class="err">{{ error }}</p>

    <div class="card">
      <h3>File (speech-to-speech / transcribe)</h3>
      <input type="file" accept="audio/wav,audio/*" @change="onFileChange" />
      <div class="row">
        <button :disabled="!file || busy" @click="runSpeechToSpeech">Speech → Speech</button>
        <button :disabled="!file || busy" @click="runTranscribe">Transcribe only</button>
      </div>

      <div v-if="s2sResult" class="result">
        <div><strong>stage:</strong> {{ s2sResult.stage }}</div>
        <div><strong>transcript:</strong> {{ s2sResult.transcript }}</div>
        <div><strong>translation:</strong> {{ s2sResult.translation }}</div>
        <div class="timings">
          stt {{ s2sResult.timings.stt_ms }}ms · mt {{ s2sResult.timings.mt_ms ?? '—' }}ms · tts
          {{ s2sResult.timings.tts_ms ?? '—' }}ms · total {{ s2sResult.timings.total_ms }}ms
        </div>
        <audio v-if="s2sAudioUrl" :src="s2sAudioUrl" controls />
      </div>

      <div v-if="transcribeResult" class="result">
        <div><strong>transcript:</strong> {{ transcribeResult.transcript }}</div>
        <div class="timings">stt {{ transcribeResult.timings.stt_ms }}ms · total {{ transcribeResult.timings.total_ms }}ms</div>
      </div>
    </div>

    <div class="card">
      <h3>Translate text</h3>
      <textarea v-model="translateText" rows="2" placeholder="Text to translate…"></textarea>
      <div class="row">
        <button :disabled="!translateText.trim() || busy" @click="runTranslate">Translate</button>
      </div>
      <div v-if="translateResult" class="result">{{ translateResult }}</div>
    </div>

    <div class="card">
      <h3>Speak text</h3>
      <textarea v-model="speakText" rows="2" placeholder="Text to synthesize…"></textarea>
      <div class="row">
        <button :disabled="!speakText.trim() || busy" @click="runSpeak">Speak</button>
      </div>
      <audio v-if="speakAudioUrl" :src="speakAudioUrl" controls />
    </div>
  </section>
</template>

<style scoped>
.stack {
  display: flex;
  flex-direction: column;
  gap: 1rem;
}
.card {
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 0.9rem 1rem;
  background: var(--surface);
}
.card h3 {
  margin: 0 0 0.5rem;
  font-size: 0.9rem;
}
.row {
  display: flex;
  gap: 0.5rem;
  margin-top: 0.5rem;
}
button {
  padding: 0.4rem 0.8rem;
  border-radius: 6px;
  border: 1px solid var(--border);
  background: var(--bg);
  cursor: pointer;
}
button[disabled] {
  opacity: 0.5;
  cursor: default;
}
textarea {
  width: 100%;
  border-radius: 6px;
  border: 1px solid var(--border);
  background: var(--bg);
  padding: 0.5rem;
  resize: vertical;
}
.result {
  margin-top: 0.75rem;
  font-size: 0.85rem;
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
}
.timings {
  color: var(--muted);
  font-size: 0.75rem;
}
.err {
  color: var(--err);
}
audio {
  margin-top: 0.5rem;
  width: 100%;
}
</style>
