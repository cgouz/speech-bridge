<script setup lang="ts">
import { onUnmounted, ref, watch } from 'vue'
import SetupPanel from './components/SetupPanel.vue'
import LiveSession from './components/LiveSession.vue'
import BatchPanel from './components/BatchPanel.vue'
import CapabilitiesView from './components/CapabilitiesView.vue'
import { useSettingsStore } from './stores/settings'
import { useHealthStore } from './stores/health'

const settings = useSettingsStore()
const health = useHealthStore()

type Tab = 'live' | 'batch' | 'observability'
const tab = ref<Tab>('live')

health.start(settings.token)
watch(() => settings.token, (t) => health.start(t))
onUnmounted(() => health.stop())
</script>

<template>
  <div class="page">
    <h1>Speech Bridge</h1>
    <p class="subtitle">Local real-time speech-to-speech translation</p>

    <SetupPanel />

    <nav class="tabs">
      <button :class="{ active: tab === 'live' }" @click="tab = 'live'">Live session</button>
      <button :class="{ active: tab === 'batch' }" @click="tab = 'batch'">Batch</button>
      <button :class="{ active: tab === 'observability' }" @click="tab = 'observability'">Observability</button>
    </nav>

    <LiveSession v-show="tab === 'live'" />
    <BatchPanel v-if="tab === 'batch'" />
    <CapabilitiesView v-if="tab === 'observability'" />
  </div>
</template>

<style scoped>
.page {
  max-width: 820px;
  margin-inline: auto;
  padding: 1.5rem;
}
h1 {
  font-size: 1.35rem;
  margin: 0;
}
.subtitle {
  margin: 0.15rem 0 1.25rem;
  color: var(--muted);
  font-size: 0.9rem;
}
.tabs {
  display: flex;
  gap: 0.25rem;
  margin-bottom: 1rem;
  border-bottom: 1px solid var(--border);
}
.tabs button {
  background: none;
  border: none;
  border-bottom: 2px solid transparent;
  padding: 0.5rem 0.75rem;
  cursor: pointer;
  color: var(--muted);
  font-size: 0.9rem;
}
.tabs button.active {
  color: var(--fg);
  border-bottom-color: var(--accent);
}
</style>
