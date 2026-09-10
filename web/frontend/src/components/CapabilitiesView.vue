<script setup lang="ts">
import { computed } from 'vue'
import { useHealthStore } from '../stores/health'

const health = useHealthStore()
const pretty = computed(() => JSON.stringify(health.capabilities, null, 2))
const readyPretty = computed(() => JSON.stringify(health.ready, null, 2))
</script>

<template>
  <section class="stack">
    <div class="card">
      <h3>/ready</h3>
      <pre>{{ readyPretty }}</pre>
    </div>
    <div class="card">
      <h3>/v1/capabilities</h3>
      <pre>{{ pretty }}</pre>
    </div>
    <p><a href="/metrics" target="_blank" rel="noopener">/metrics (Prometheus)</a></p>
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
pre {
  margin: 0;
  font-size: 0.8rem;
  overflow-x: auto;
  white-space: pre-wrap;
  word-break: break-word;
}
a {
  color: var(--accent);
}
</style>
