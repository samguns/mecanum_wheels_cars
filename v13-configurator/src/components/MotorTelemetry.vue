<script setup lang="ts">
import type { MotorRecord } from '@/types/protocol'

defineProps<{ motors: MotorRecord[]; stale: boolean }>()
</script>

<template>
  <section class="panel">
    <h2>Motors <span v-if="stale" class="derived">stale</span></h2>
    <table>
      <thead>
        <tr>
          <th>M</th>
          <th>armed</th>
          <th>mode</th>
          <th>pos mrad</th>
          <th>vel rad/s</th>
          <th>iq A</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="m in motors" :key="m.motor">
          <td class="reported">{{ m.motor }}</td>
          <td class="reported">{{ m.armed }}</td>
          <td class="reported">{{ m.mode }}</td>
          <td class="reported">{{ m.position_mrad }}</td>
          <td class="reported">{{ m.velocity.toFixed(3) }}</td>
          <td class="reported">{{ m.current_q.toFixed(3) }}</td>
        </tr>
      </tbody>
    </table>
  </section>
</template>

<style scoped>
.panel {
  background: #12181f;
  border: 1px solid #1d242c;
  border-radius: 8px;
  padding: 16px;
  margin-bottom: 16px;
}
table {
  width: 100%;
  font-size: 13px;
}
th {
  color: #6b7681;
  text-align: left;
}
.reported {
  color: #e6edf3;
}
.derived {
  font-style: italic;
  color: #7dd3c7;
}
</style>
