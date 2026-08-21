<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { invoke } from '@tauri-apps/api/core'

interface ChangeLogEntry {
  timestamp: string
  operator: string
  can_id: number
  motor_index: number | null
  wheel_label: string
  kind: string
  before: unknown
  after: unknown
}

const entries = ref<ChangeLogEntry[]>([])
const canFilter = ref('')
const motorFilter = ref('')

onMounted(async () => {
  try {
    entries.value = await invoke<ChangeLogEntry[]>('change_log_read')
  } catch {
    entries.value = []
  }
})

function formatValue(value: unknown) {
  return JSON.stringify(value)
}

function shown(entry: ChangeLogEntry) {
  if (canFilter.value) {
    const id = Number.parseInt(canFilter.value, 16)
    if (Number.isFinite(id) && entry.can_id !== id) return false
  }
  if (motorFilter.value) {
    if (String(entry.motor_index) !== motorFilter.value) return false
  }
  return true
}
</script>

<template>
  <section>
    <h2>Change history</h2>
    <div class="filters">
      <label>Controller hex <input v-model="canFilter" /></label>
      <label>Motor <input v-model="motorFilter" /></label>
    </div>
    <ul>
      <li v-for="(entry, i) in entries.filter(shown)" :key="i">
        <strong>{{ entry.kind }}</strong>
        · {{ entry.operator }} · 0x{{ entry.can_id.toString(16) }} · {{ entry.wheel_label }}
        · {{ entry.timestamp }}
        <pre class="values">before {{ formatValue(entry.before) }}
after  {{ formatValue(entry.after) }}</pre>
      </li>
    </ul>
    <p v-if="!entries.length">No acknowledged writes or accepted calibrations yet.</p>
  </section>
</template>

<style scoped>
h2 {
  font-size: 16px;
}
.filters {
  display: flex;
  gap: 12px;
  margin-bottom: 12px;
}
input {
  background: #0b0f14;
  color: inherit;
  border: 1px solid #2a333d;
}
li {
  font-size: 13px;
  color: #c5cdd4;
  margin-bottom: 12px;
}
.values {
  margin: 4px 0 0;
  font-size: 11px;
  color: #8b959f;
  white-space: pre-wrap;
}
</style>
