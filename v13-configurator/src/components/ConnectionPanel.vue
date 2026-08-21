<script setup lang="ts">
import { onMounted } from 'vue'

import { useSessionStore } from '@/stores/session'

const session = useSessionStore()

onMounted(() => {
  void session.listPorts()
})
</script>

<template>
  <section class="panel">
    <h2>Connection</h2>
    <div class="row">
      <select v-model="session.selectedPort" :disabled="session.connection !== 'disconnected'">
        <option value="" disabled>Select port</option>
        <option v-for="port in session.ports" :key="port.name" :value="port.name">
          {{ port.name }}
        </option>
      </select>
      <button
        v-if="session.connection === 'disconnected'"
        type="button"
        @click="session.connect()"
      >
        Connect
      </button>
      <button v-else type="button" @click="session.disconnect()">Disconnect</button>
      <button type="button" class="ghost" @click="session.listPorts()">Refresh ports</button>
    </div>
    <p class="state">State: {{ session.connection }}</p>
    <p v-if="session.identity" class="id">
      {{ session.identity.firmware_level }} · proto {{ session.identity.protocol_version }} ·
      CAN 0x{{ session.identity.can_id.toString(16).toUpperCase() }} · cfg
      {{ session.identity.config_version }}
    </p>
    <p v-if="session.resetForced" class="warn">
      The controller reset when the port opened. Identity is from the new boot, not the previous
      session.
    </p>
    <p v-if="session.lastError" class="warn">{{ session.lastError.reason }}</p>
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
  margin: 0 0 12px;
  font-size: 14px;
}
.row {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
}
select,
button {
  background: #0b0f14;
  color: inherit;
  border: 1px solid #2a333d;
  border-radius: 6px;
  padding: 6px 10px;
}
button {
  cursor: pointer;
}
.ghost {
  opacity: 0.8;
}
.state,
.id {
  margin: 8px 0 0;
  font-size: 12px;
  color: #8b959f;
}
.warn {
  color: #e3b341;
  font-size: 12px;
}
</style>
