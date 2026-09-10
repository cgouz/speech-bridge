import { defineStore } from 'pinia'
import { ref, watch } from 'vue'

const TOKEN_KEY = 'sb.authToken'
const SRC_KEY = 'sb.sourceLang'
const DST_KEY = 'sb.targetLang'
const VOICE_KEY = 'sb.voice'

function load(key: string, fallback: string): string {
  try {
    return localStorage.getItem(key) ?? fallback
  } catch {
    return fallback
  }
}

function persist(key: string, value: string): void {
  try {
    localStorage.setItem(key, value)
  } catch {
    // private browsing / storage disabled — settings just won't survive reload
  }
}

export const useSettingsStore = defineStore('settings', () => {
  const token = ref(load(TOKEN_KEY, ''))
  const sourceLang = ref(load(SRC_KEY, 'ru'))
  const targetLang = ref(load(DST_KEY, 'uz'))
  const voice = ref(load(VOICE_KEY, ''))

  watch(token, (v) => persist(TOKEN_KEY, v))
  watch(sourceLang, (v) => persist(SRC_KEY, v))
  watch(targetLang, (v) => persist(DST_KEY, v))
  watch(voice, (v) => persist(VOICE_KEY, v))

  return { token, sourceLang, targetLang, voice }
})
