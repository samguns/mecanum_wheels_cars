<script setup lang="ts">
import type { BusRecord } from '@/types/protocol'

defineProps<{ bus: BusRecord | null }>()
</script>

<template>
  <section class="panel">
    <h2>Bus</h2>
    <p v-if="bus" class="reported">{{ (bus.millivolts / 1000).toFixed(2) }} V · protect {{ bus.protection_active }}</p>
    <p v-else>No bus record yet.</p>
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
.reported {
  color: #e6edf3;
}
</style>
