<script setup lang="ts">
import type { CfgRecord, TimingRecord } from '@/types/protocol'

defineProps<{ timing: TimingRecord | null; cfg: CfgRecord | null }>()
</script>

<template>
  <section class="panel">
    <h2>Timing</h2>
    <dl v-if="timing">
      <div><dt>Nominal / measured</dt><dd class="reported">{{ timing.rate_nominal_hz }} / {{ timing.rate_measured_hz.toFixed(1) }} Hz</dd></div>
      <div><dt>Overruns / worst</dt><dd class="reported">{{ timing.overruns }} / {{ timing.worst_cycle_us }} µs</dd></div>
      <div><dt>Duty</dt><dd class="derived">{{ timing.duty_percent.toFixed(1) }} %</dd></div>
      <div v-if="cfg"><dt>Carrier / BW</dt><dd class="reported">{{ cfg.carrier_hz }} Hz · {{ cfg.bandwidth_requested_hz }}→{{ cfg.bandwidth_active_hz }} {{ cfg.bandwidth_clamped ? 'CLAMPED' : '' }}</dd></div>
    </dl>
    <p v-else>No timing record yet.</p>
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
.derived {
  font-style: italic;
  color: #7dd3c7;
}
dt {
  font-size: 11px;
  color: #6b7681;
}
dd {
  margin: 0 0 8px;
}
</style>
