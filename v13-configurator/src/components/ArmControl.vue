<script setup lang="ts">
import { computed } from 'vue'

import { wheelLabel } from '@/lib/wheel'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'

const session = useSessionStore()
const device = useDeviceStore()

const selectedWheel = computed(() =>
  wheelLabel(session.identity?.can_id ?? 0, session.selectedMotor),
)

const selectedArmed = computed(
  () => device.mirror.motors.find((m) => m.motor === session.selectedMotor)?.armed ?? false,
)
</script>

<template>
  <div class="arm-ctl">
    <button
      type="button"
      class="arm"
      :disabled="!session.canAct || selectedArmed"
      @click="session.arm()"
    >
      Arm {{ selectedWheel }}
    </button>
    <button
      type="button"
      class="disarm"
      :disabled="!session.canAct"
      @click="session.disarm()"
    >
      Disarm {{ selectedWheel }}
    </button>
  </div>
</template>

<style scoped>
.arm-ctl {
  display: flex;
  gap: 6px;
}
button {
  border-radius: 6px;
  padding: 6px 10px;
  font-size: 12px;
  cursor: pointer;
}
button:disabled {
  opacity: 0.4;
  cursor: default;
}
.arm {
  background: #16323a;
  color: #4fd6c1;
  border: 1px solid #2a4a50;
}
.disarm {
  background: #0b0f14;
  color: inherit;
  border: 1px solid #2a333d;
}
</style>
