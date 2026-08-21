<script setup lang="ts">
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
</script>

<template>
  <button
    type="button"
    class="stop"
    :disabled="session.connection === 'disconnected'"
    @click="session.abort()"
  >
    Stop
  </button>
</template>

<style scoped>
.stop {
  background: #4a1515;
  color: #ff8a8a;
  border: 1px solid #8a2b2b;
  border-radius: 6px;
  padding: 8px 16px;
  font-weight: 600;
  cursor: pointer;
}
.stop:disabled {
  opacity: 0.4;
  cursor: default;
}
</style>
