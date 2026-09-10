<script setup lang="ts">
import { computed } from 'vue'
import { useSettingsStore } from '../stores/settings'
import { useHealthStore } from '../stores/health'

const settings = useSettingsStore()
const health = useHealthStore()

const STT_FALLBACK = ['uz', 'ru', 'kaa', 'auto']
const sourceLangs = computed(() => health.capabilities?.stt_langs ?? STT_FALLBACK)

// Target options: union of every language any loaded TTS engine covers, plus
// the STT languages (captions-only targets still make sense, e.g. translate
// without speaking). Degraded (caption-only) targets get a "no voice" badge.
const targetLangs = computed(() => {
  const caps = health.capabilities
  const set = new Set<string>(['uz', 'ru', 'en', 'de'])
  if (caps) {
    for (const l of caps.tts.magpie ?? []) set.add(l)
    for (const l of caps.tts.vits ?? []) set.add(l)
    for (const l of caps.stt_langs ?? []) if (l !== 'auto') set.add(l)
  }
  return [...set].sort()
})

const readyBadge = computed(() => {
  if (!health.ready) return { cls: 'warn', text: 'checking…' }
  return health.ready.ready ? { cls: 'ok', text: 'ready' } : { cls: 'warn', text: 'not ready' }
})
</script>

<template>
  <section class="panel">
    <div class="row">
      <label>
        From
        <select v-model="settings.sourceLang">
          <option v-for="l in sourceLangs" :key="l" :value="l">{{ l }}</option>
        </select>
      </label>
      <label>
        To
        <select v-model="settings.targetLang">
          <option v-for="l in targetLangs" :key="l" :value="l">{{ l }}</option>
        </select>
      </label>
      <span v-if="!health.hasVoice(settings.targetLang)" class="badge warn" title="No TTS engine covers this target — captions only">
        no voice
      </span>
      <label>
        Voice
        <input v-model="settings.voice" size="10" placeholder="default" />
      </label>
    </div>
    <div class="row">
      <label class="grow">
        Auth token
        <input v-model="settings.token" type="password" placeholder="SB_AUTH_TOKEN (leave empty if auth is off)" />
      </label>
      <span class="badge" :class="readyBadge.cls">{{ readyBadge.text }}</span>
    </div>
    <p v-if="health.lastError" class="err">{{ health.lastError }}</p>
  </section>
</template>

<style scoped>
.panel {
  display: flex;
  flex-direction: column;
  gap: 0.6rem;
  padding: 0.9rem 1rem;
  border: 1px solid var(--border);
  border-radius: 10px;
  background: var(--surface);
  margin-bottom: 1rem;
}
.row {
  display: flex;
  gap: 0.75rem;
  flex-wrap: wrap;
  align-items: center;
}
label {
  display: flex;
  flex-direction: column;
  gap: 0.2rem;
  font-size: 0.75rem;
  color: var(--muted);
}
label.grow {
  flex: 1;
  min-width: 16rem;
}
select,
input {
  padding: 0.4rem 0.5rem;
  border-radius: 6px;
  border: 1px solid var(--border);
  background: var(--bg);
}
.err {
  color: var(--err);
  font-size: 0.8rem;
  margin: 0;
}
</style>
