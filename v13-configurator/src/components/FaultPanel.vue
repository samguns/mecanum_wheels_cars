<script setup lang="ts">
import type { MotorRecord, TimingRecord } from '@/types/protocol'

defineProps<{ motors: MotorRecord[]; timing?: TimingRecord | null }>()
</script>

<template>
  <section class="panel">
    <h2>Faults and limits</h2>
    <ul>
      <li v-for="m in motors" :key="m.motor">
        M{{ m.motor }}
        current {{ m.limit_causes.current }} ·
        output voltage
        {{ m.limit_causes.output_voltage === null ? 'unavailable' : m.limit_causes.output_voltage }}
        · bus {{ m.limit_causes.bus_voltage }} · timeout {{ m.timed_out }} · pair fault
        {{ m.pair_fault === null ? 'unavailable' : m.pair_fault }}
      </li>
    </ul>
    <p class="reported">Timing fault: {{ timing?.fault ?? 'none' }}</p>
  </section>
</template>

<style scoped>
.panel {
  background: #12181f;
  border: 1px solid #1d242c;
  border-radius: 8px;
  padding: 16px;
}
li,
.reported {
  font-size: 13px;
}
</style>
