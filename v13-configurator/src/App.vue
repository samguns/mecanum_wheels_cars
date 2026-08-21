<script setup lang="ts">
import { computed, ref } from 'vue'

import ArmControl from '@/components/ArmControl.vue'
import MotorSelector from '@/components/MotorSelector.vue'
import OperatorFooter from '@/components/OperatorFooter.vue'
import StopControl from '@/components/StopControl.vue'
import ConfigView from '@/views/ConfigView.vue'
import DebugView from '@/views/DebugView.vue'
import HistoryView from '@/views/HistoryView.vue'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'
import { wheelLabel } from '@/lib/wheel'

type Route = 'config' | 'debug' | 'history'

const route = ref<Route>('config')
const session = useSessionStore()
const device = useDeviceStore()

const selectedWheel = computed(() =>
  wheelLabel(session.identity?.can_id ?? 0, session.selectedMotor),
)

function motorArmed(motor: number) {
  return device.mirror.motors.find((m) => m.motor === motor)?.armed ?? false
}
</script>

<template>
  <div class="app">
    <aside class="sidebar">
      <div class="brand">V13-Driver</div>

      <div class="nav-group">
        <div class="nav-label">MAIN</div>
        <button :class="{ active: route === 'config' }" @click="route = 'config'">BLDC Config</button>
      </div>

      <div class="nav-group">
        <div class="nav-label">DEBUG</div>
        <button :class="{ active: route === 'debug' }" @click="route = 'debug'">Control</button>
        <button :class="{ active: route === 'history' }" @click="route = 'history'">History</button>
      </div>
      <OperatorFooter />
    </aside>

    <main class="content">
      <header class="chrome">
        <div>
          <h1>BLDC Configurator</h1>
          <p class="subtitle">
            <span v-if="session.identity">
              0x{{ session.identity.can_id.toString(16).toUpperCase() }} ·
              {{ selectedWheel }}
            </span>
            <span v-else>Brushless DC Motor Diagnostic &amp; tuning Suite</span>
          </p>
        </div>
        <div class="chrome-actions">
          <MotorSelector />
          <div class="arms">
            <span :class="{ hot: device.progress?.energised }">
              {{ device.progress?.energised ? 'energised' : 'de-energised' }}
            </span>
            <span :class="{ hot: motorArmed(1) }">M1 {{ motorArmed(1) ? 'armed' : 'disarmed' }}</span>
            <span :class="{ hot: motorArmed(2) }">M2 {{ motorArmed(2) ? 'armed' : 'disarmed' }}</span>
          </div>
          <ArmControl />
          <StopControl />
        </div>
      </header>
      <ConfigView v-if="route === 'config'" />
      <DebugView v-else-if="route === 'debug'" />
      <HistoryView v-else />
    </main>
  </div>
</template>

<style scoped>
.app {
  display: flex;
  min-height: 100vh;
  background: #0b0f14;
  color: #e6edf3;
  font-family: ui-sans-serif, system-ui, sans-serif;
}
.sidebar {
  width: 220px;
  border-right: 1px solid #1d242c;
  padding: 16px;
  display: flex;
  flex-direction: column;
}
.brand {
  font-weight: 600;
  margin-bottom: 24px;
}
.nav-label {
  font-size: 11px;
  letter-spacing: 0.08em;
  color: #6b7681;
  margin: 16px 0 6px;
}
.sidebar button {
  display: block;
  width: 100%;
  text-align: left;
  background: transparent;
  border: 0;
  color: inherit;
  padding: 8px 10px;
  border-radius: 6px;
  cursor: pointer;
}
.sidebar button.active {
  background: #16323a;
  color: #4fd6c1;
}
.content {
  flex: 1;
  padding: 24px 32px;
}
.chrome {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  margin-bottom: 20px;
  position: sticky;
  top: 0;
  background: #0b0f14;
  z-index: 2;
  padding-bottom: 8px;
}
.chrome-actions {
  display: flex;
  align-items: center;
  gap: 12px;
}
.arms {
  display: flex;
  gap: 8px;
  font-size: 12px;
  color: #8b959f;
}
.arms .hot {
  color: #ff8a8a;
}
h1 {
  font-size: 20px;
  margin: 0;
}
.subtitle {
  color: #6b7681;
  font-size: 12px;
  margin: 4px 0 0;
}
</style>
