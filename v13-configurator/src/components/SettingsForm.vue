<script setup lang="ts">
import { computed, ref } from 'vue'

import RefusalNotice from '@/components/RefusalNotice.vue'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'
import { invoke } from '@tauri-apps/api/core'

const device = useDeviceStore()
const session = useSessionStore()

const canId = ref('202')
const bandwidth = ref(1000)
const busMin = ref(7000)
const busMax = ref(24000)
const mode1 = ref<'velocity' | 'impedance'>('velocity')
const mode2 = ref<'velocity' | 'impedance'>('velocity')
const confirm = ref(false)
const refusal = ref('')

const armed = computed(() => device.mirror.motors.some((m) => m.armed))

const rangeError = computed(() => {
  const id = Number.parseInt(canId.value, 16)
  if (!Number.isFinite(id) || id < 0x001 || id > 0x7ff) return 'CAN id must be 0x001-0x7FF'
  if (bandwidth.value < 100 || bandwidth.value > 10000) return 'bandwidth must be 100-10000 Hz'
  if (busMin.value >= busMax.value) return 'bus window min must be less than max'
  if (busMin.value < 7000 || busMax.value > 24000) return 'bus window must be 7000-24000 mV'
  return ''
})

async function write(command: string) {
  refusal.value = ''
  if (armed.value) {
    refusal.value = 'disarm both motors first'
    return
  }
  if (rangeError.value) {
    refusal.value = rangeError.value
    return
  }
  if (!confirm.value) {
    refusal.value = 'confirm the before/after comparison first'
    return
  }
  try {
    await invoke('write_setting', { args: { command } })
    await session.readAll()
  } catch (err) {
    refusal.value = err && typeof err === 'object' && 'reason' in err ? String((err as { reason: string }).reason) : String(err)
  }
}
</script>

<template>
  <section class="panel">
    <h2>Writable settings</h2>
    <p v-if="armed" class="warn">A disarm is required before any write.</p>
    <label>CAN id (hex) <input v-model="canId" /></label>
    <label>Bandwidth Hz <input v-model.number="bandwidth" type="number" min="100" max="10000" /></label>
    <label>Bus min mV <input v-model.number="busMin" type="number" /></label>
    <label>Bus max mV <input v-model.number="busMax" type="number" /></label>
    <label>Motor 1 mode
      <select v-model="mode1">
        <option value="velocity">velocity</option>
        <option value="impedance">impedance</option>
      </select>
    </label>
    <label>Motor 2 mode
      <select v-model="mode2">
        <option value="velocity">velocity</option>
        <option value="impedance">impedance</option>
      </select>
    </label>
    <p class="compare reported">
      Before: id 0x{{ (device.mirror.cfg?.can_id ?? 0).toString(16) }} bw
      {{ device.mirror.cfg?.bandwidth_requested_hz }} window
      {{ device.mirror.cfg?.bus_min_mv }}/{{ device.mirror.cfg?.bus_max_mv }}
      modes {{ device.mirror.cfg?.mode?.join('/') }}
    </p>
    <p class="compare derived">
      After (if confirmed): id 0x{{ canId }} bw {{ bandwidth }} window {{ busMin }}/{{ busMax }}
      modes {{ mode1 }}/{{ mode2 }}
    </p>
    <label><input v-model="confirm" type="checkbox" /> Confirm write</label>
    <p v-if="rangeError" class="warn">{{ rangeError }}</p>
    <RefusalNotice v-if="refusal" :reason="refusal" />
    <div class="row">
      <button type="button" @click="write('N' + canId)">Write identity</button>
      <button type="button" @click="write('B' + bandwidth)">Write bandwidth</button>
      <button type="button" @click="write('V' + busMin + ',' + busMax)">Write bus window</button>
      <button type="button" @click="write('M1' + (mode1 === 'impedance' ? 'I' : 'V'))">Write M1 mode</button>
      <button type="button" @click="write('M2' + (mode2 === 'impedance' ? 'I' : 'V'))">Write M2 mode</button>
    </div>
  </section>
</template>

<style scoped>
.panel {
  background: #12181f;
  border: 1px solid #1d242c;
  border-radius: 8px;
  padding: 16px;
  margin-top: 16px;
}
label {
  display: block;
  font-size: 12px;
  margin: 6px 0;
}
input,
select {
  margin-left: 8px;
  background: #0b0f14;
  color: inherit;
  border: 1px solid #2a333d;
}
.warn {
  color: #e3b341;
}
.row {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
button {
  background: #16323a;
  color: #4fd6c1;
  border: 0;
  border-radius: 6px;
  padding: 6px 10px;
}
</style>
