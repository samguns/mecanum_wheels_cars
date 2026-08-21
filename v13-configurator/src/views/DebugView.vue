<script setup lang="ts">
import BusVoltagePanel from '@/components/BusVoltagePanel.vue'
import FaultPanel from '@/components/FaultPanel.vue'
import ImpedancePanel from '@/components/ImpedancePanel.vue'
import MotorTelemetry from '@/components/MotorTelemetry.vue'
import TimingPanel from '@/components/TimingPanel.vue'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'
import { ref } from 'vue'

const device = useDeviceStore()
const session = useSessionStore()
const applied = ref<number | null>(null)

async function enableTelemetry() {
  applied.value = await session.setTelemetry(200)
}
</script>

<template>
  <div>
    <p class="lead">DEBUG · live values from the controller, never inferred.</p>
    <button type="button" :disabled="!session.canAct" @click="enableTelemetry">Start 200 ms telemetry</button>
    <p v-if="applied !== null" class="lead">Applied period {{ applied }} ms</p>
    <MotorTelemetry :motors="device.mirror.motors" :stale="device.stale" />
    <ImpedancePanel />
    <BusVoltagePanel :bus="device.mirror.bus" />
    <TimingPanel :timing="device.mirror.timing" :cfg="device.mirror.cfg" />
    <FaultPanel :motors="device.mirror.motors" :timing="device.mirror.timing" />
  </div>
</template>

<style scoped>
.lead {
  color: #8b959f;
  font-size: 13px;
}
button {
  margin-bottom: 16px;
  background: #16323a;
  color: #4fd6c1;
  border: 0;
  border-radius: 6px;
  padding: 6px 12px;
}
</style>
