<script setup lang="ts">
import { computed } from 'vue'

import { wheelLabel } from '@/lib/wheel'
import type { CalRecord } from '@/types/protocol'

const props = defineProps<{
  motors: CalRecord[]
  canId: number
}>()

const rows = computed(() =>
  [1, 2].map((motor) => {
    const cal = props.motors.find((c) => c.motor === motor)
    const complete = Boolean(cal?.aligned && cal?.characterised && cal?.valid)
    return {
      motor,
      wheel: wheelLabel(props.canId, motor),
      cal,
      complete,
    }
  }),
)

const incomplete = computed(() => rows.value.filter((r) => !r.complete))
</script>

<template>
  <section class="panel">
    <h2>Calibration state</h2>
    <p v-if="incomplete.length" class="warn">
      Requires calibration:
      {{ incomplete.map((r) => r.wheel).join(', ') }}
    </p>
    <p v-else class="ok">Both motors report a valid stored calibration.</p>
    <ul>
      <li v-for="row in rows" :key="row.motor">
        <strong class="derived">{{ row.wheel }}</strong>
        <span class="reported">
          align {{ row.cal?.aligned ? 'done' : 'open' }} · charac
          {{ row.cal?.characterised ? 'done' : 'open' }} · valid
          {{ row.cal?.valid ? 'yes' : 'no' }}
        </span>
      </li>
    </ul>
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
h2 {
  margin: 0 0 8px;
  font-size: 14px;
}
ul {
  margin: 0;
  padding: 0;
  list-style: none;
  font-size: 13px;
}
li {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  padding: 4px 0;
}
.derived {
  font-style: italic;
  color: #7dd3c7;
}
.reported {
  color: #c5cdd4;
}
.warn {
  color: #e3b341;
  font-size: 13px;
}
.ok {
  color: #4fd6c1;
  font-size: 13px;
}
</style>
