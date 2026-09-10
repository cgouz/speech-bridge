import { defineStore } from 'pinia'
import { ref } from 'vue'
import { getCapabilities, getReady } from '../api/client'
import type { CapabilitiesResponse, ReadyResponse } from '../api/types'

const POLL_MS = 5000

export const useHealthStore = defineStore('health', () => {
  const ready = ref<ReadyResponse | null>(null)
  const capabilities = ref<CapabilitiesResponse | null>(null)
  const lastError = ref('')
  let timer: ReturnType<typeof setInterval> | null = null

  async function poll(token: string): Promise<void> {
    try {
      ready.value = await getReady()
      capabilities.value = await getCapabilities(token)
      lastError.value = ''
    } catch (e) {
      lastError.value = e instanceof Error ? e.message : String(e)
    }
  }

  function start(token: string): void {
    stop()
    void poll(token)
    timer = setInterval(() => void poll(token), POLL_MS)
  }

  function stop(): void {
    if (timer) clearInterval(timer)
    timer = null
  }

  /** Targets with real voice coverage from a loaded TTS engine (not just captions). */
  function hasVoice(lang: string): boolean {
    const tts = capabilities.value?.tts
    if (!tts) return false
    return (tts.magpie ?? []).includes(lang) || (tts.vits ?? []).includes(lang)
  }

  return { ready, capabilities, lastError, start, stop, poll, hasVoice }
})
