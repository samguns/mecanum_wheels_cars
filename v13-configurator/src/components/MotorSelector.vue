<script setup lang="ts">
import { wheelLabel } from '@/lib/wheel'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()

function label(motor: 1 | 2) {
  return wheelLabel(session.identity?.can_id ?? 0, motor)
}
</script>

<template>
  <div class="selector">
    <button
      v-for="motor in [1, 2] as const"
      :key="motor"
      type="button"
      :class="{ active: session.selectedMotor === motor }"
      @click="session.selectMotor(motor)"
    >
      {{ label(motor) }}
    </button>
  </div>
</template>

<style scoped>
.selector {
  display: flex;
  gap: 8px;
}
button {
  background: #0b0f14;
  color: inherit;
  border: 1px solid #2a333d;
  border-radius: 6px;
  padding: 6px 10px;
  cursor: pointer;
}
button.active {
  background: #16323a;
  color: #4fd6c1;
}
</style>
