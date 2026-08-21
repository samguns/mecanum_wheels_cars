<script setup lang="ts">
import { computed, ref, watch } from 'vue'

import RefusalNotice from '@/components/RefusalNotice.vue'
import { inRange, RANGES } from '@/lib/ranges'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'

const device = useDeviceStore()
const session = useSessionStore()

const pDes = ref(0)
const vDes = ref(0)
const kp = ref(0)
const kd = ref(0)
const tFf = ref(0)
const localRefusal = ref('')

const selectedImp = computed(() =>
  device.mirror.impedance.find((r) => r.motor === session.selectedMotor) ?? null,
)
const selectedMotor = computed(() =>
  device.mirror.motors.find((m) => m.motor === session.selectedMotor) ?? null,
)
const impedanceMode = computed(() => selectedMotor.value?.mode === 'impedance')

const rangeError = computed(() => {
  if (!inRange(pDes.value, RANGES.p_des_mrad.min, RANGES.p_des_mrad.max)) {
    return 'p_des must be -628319 to 628319 mrad'
  }
  if (!inRange(vDes.value, RANGES.v_des.min, RANGES.v_des.max)) {
    return 'v_des must be -45 to 45 rad/s'
  }
  if (!inRange(kp.value, RANGES.kp.min, RANGES.kp.max)) return 'kp must be 0 to 50'
  if (!inRange(kd.value, RANGES.kd.min, RANGES.kd.max)) return 'kd must be 0 to 1'
  if (!inRange(tFf.value, RANGES.t_ff.min, RANGES.t_ff.max)) return 't_ff must be -0.5 to 0.5 Nm'
  return ''
})

watch(
  selectedImp,
  (record) => {
    if (!record) return
    pDes.value = record.p_des_mrad
    vDes.value = record.v_des
    kp.value = record.kp
    kd.value = record.kd
    tFf.value = record.t_ff
  },
  { immediate: true },
)

async function apply() {
  localRefusal.value = ''
  if (!impedanceMode.value) {
    localRefusal.value = 'motor not in impedance mode'
    return
  }
  if (rangeError.value) {
    localRefusal.value = rangeError.value
    return
  }
  try {
    await session.applyImpedance({
      motor: session.selectedMotor,
      p_des_mrad: pDes.value,
      v_des: vDes.value,
      kp: kp.value,
      kd: kd.value,
      t_ff: tFf.value,
    })
  } catch {
    /* session.refuseReason already set */
  }
}
</script>

<template>
  <section class="panel">
    <h2>Impedance <span v-if="device.stale" class="derived">stale</span></h2>
    <p class="lead">
      Serial stand-in for the CAN pair. Apply after arming; the setpoint is held until disarm.
      Applying while armed produces torque on the selected wheel.
    </p>
    <table>
      <thead>
        <tr>
          <th>M</th>
          <th>eligible</th>
          <th>p_des</th>
          <th>kp</th>
          <th>kd</th>
          <th>t_ff</th>
          <th>err rad</th>
          <th>tq Nm</th>
          <th>hold</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="r in device.mirror.impedance" :key="r.motor">
          <td class="reported">{{ r.motor }}</td>
          <td class="reported">{{ r.eligible }}</td>
          <td class="reported">{{ r.p_des_mrad }}</td>
          <td class="reported">{{ r.kp.toFixed(3) }}</td>
          <td class="reported">{{ r.kd.toFixed(3) }}</td>
          <td class="reported">{{ r.t_ff.toFixed(4) }}</td>
          <td class="reported">{{ r.position_error.toFixed(5) }}</td>
          <td class="reported">{{ r.torque_cmd.toFixed(4) }}</td>
          <td class="reported">{{ r.serial_hold }}</td>
        </tr>
      </tbody>
    </table>
    <div class="form">
      <label>p_des mrad <input v-model.number="pDes" type="number" /></label>
      <label>v_des <input v-model.number="vDes" type="number" step="0.001" /></label>
      <label>kp <input v-model.number="kp" type="number" step="0.001" /></label>
      <label>kd <input v-model.number="kd" type="number" step="0.001" /></label>
      <label>t_ff <input v-model.number="tFf" type="number" step="0.0001" /></label>
      <button type="button" :disabled="!session.canAct || !impedanceMode" @click="apply">
        Apply M{{ session.selectedMotor }}
      </button>
    </div>
    <p v-if="!impedanceMode" class="lead">Selected motor is not in impedance mode (use M1I / M2I).</p>
    <RefusalNotice
      v-if="localRefusal || session.refuseReason"
      :reason="localRefusal || session.refuseReason || ''"
    />
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
.lead {
  color: #8b959f;
  font-size: 13px;
}
table {
  width: 100%;
  font-size: 13px;
  margin-bottom: 12px;
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
.form {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: end;
}
label {
  color: #8b959f;
  font-size: 12px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}
input {
  width: 88px;
  background: #0b0f14;
  color: #e6edf3;
  border: 1px solid #2a333d;
  border-radius: 4px;
  padding: 4px 6px;
}
button {
  background: #16323a;
  color: #4fd6c1;
  border: 0;
  border-radius: 6px;
  padding: 6px 12px;
}
button:disabled {
  opacity: 0.4;
}
</style>
