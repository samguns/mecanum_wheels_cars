<script setup lang="ts">
export interface PendingValues {
  motor: number
  stage: string
  pole_pairs?: number | null
  direction?: number | null
  electrical_offset?: number | null
  phase_resistance?: number | null
  inductance_d?: number | null
  inductance_q?: number | null
}

defineProps<{ pending: PendingValues }>()
</script>

<template>
  <section class="panel">
    <h2>Pending result — not stored</h2>
    <p class="warn">
      These values are measured only. They become stored after a successful accept acknowledgement.
    </p>
    <dl class="reported">
      <div><dt>Motor / stage</dt><dd>M{{ pending.motor }} {{ pending.stage }}</dd></div>
      <div v-if="pending.pole_pairs != null"><dt>Pole pairs</dt><dd>{{ pending.pole_pairs }}</dd></div>
      <div v-if="pending.direction != null"><dt>Direction</dt><dd>{{ pending.direction }}</dd></div>
      <div v-if="pending.electrical_offset != null"><dt>Offset</dt><dd>{{ pending.electrical_offset }} rad</dd></div>
      <div v-if="pending.phase_resistance != null"><dt>R</dt><dd>{{ pending.phase_resistance }} Ω</dd></div>
      <div v-if="pending.inductance_d != null"><dt>Ld</dt><dd>{{ pending.inductance_d }} H</dd></div>
      <div v-if="pending.inductance_q != null"><dt>Lq</dt><dd>{{ pending.inductance_q }} H</dd></div>
    </dl>
  </section>
</template>

<style scoped>
.panel {
  background: #1a1610;
  border: 1px solid #e3b341;
  border-radius: 8px;
  padding: 16px;
  margin: 16px 0;
}
.warn {
  color: #e3b341;
  font-size: 13px;
}
.reported {
  color: #e6edf3;
}
dt {
  font-size: 11px;
  color: #6b7681;
}
dd {
  margin: 0 0 8px;
}
</style>
