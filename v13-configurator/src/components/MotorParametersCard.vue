<script setup lang="ts">
import { computed } from 'vue'

import { fieldFlagged, pairMismatched } from '@/lib/ranges'
import type { CalRecord, CfgRecord } from '@/types/protocol'

const props = defineProps<{
  cal: CalRecord
  cfg: CfgRecord | null
  wheelLabel: string
  reference?: CalRecord | null
  referenceLabel?: string
  freshnessAgeMs: number | null
  dutyPercent: number | null
}>()

const flagged = computed(
  () =>
    fieldFlagged('pole_pairs', props.cal, props.reference) ||
    fieldFlagged('direction', props.cal, props.reference) ||
    fieldFlagged('electrical_offset', props.cal, props.reference) ||
    fieldFlagged('phase_resistance', props.cal, props.reference) ||
    fieldFlagged('inductance_d', props.cal, props.reference) ||
    fieldFlagged('inductance_q', props.cal, props.reference),
)

const pairDiffers = computed(() => pairMismatched(props.cal, props.reference))

function fmt(value: number, digits = 6): string {
  return Number.isFinite(value) ? value.toFixed(digits) : String(value)
}

function age(ms: number | null): string {
  if (ms === null) return 'never'
  if (ms < 1000) return `${ms} ms`
  return `${(ms / 1000).toFixed(1)} s`
}
</script>

<template>
  <article class="card" :data-motor="cal.motor">
    <header>
      <h2 class="derived">{{ wheelLabel }}</h2>
      <span class="derived flag">out of range: {{ flagged ? 'yes' : 'no' }}</span>
    </header>
    <p class="hint reported">
      These values are stored on the controller. Changing them requires running calibration.
    </p>
    <p v-if="pairDiffers && referenceLabel" class="hint warn">
      Differs from {{ referenceLabel }} (same-spec pair): pole pairs must match; R and L should be
      within 25%.
    </p>
    <dl>
      <div :class="{ oor: fieldFlagged('pole_pairs', cal, reference) }">
        <dt>Pole pairs</dt>
        <dd class="reported">{{ cal.pole_pairs }}</dd>
      </div>
      <div :class="{ oor: fieldFlagged('direction', cal, reference) }">
        <dt>Direction</dt>
        <dd class="reported">{{ cal.direction === 1 ? '+1 CW' : cal.direction === -1 ? '−1 CCW' : cal.direction }}</dd>
      </div>
      <div :class="{ oor: fieldFlagged('phase_resistance', cal, reference) }">
        <dt>Resistance</dt>
        <dd class="reported">{{ fmt(cal.phase_resistance, 4) }} Ω</dd>
      </div>
      <div :class="{ oor: fieldFlagged('inductance_d', cal, reference) }">
        <dt>Inductance D</dt>
        <dd class="reported">{{ fmt(cal.inductance_d, 8) }} H</dd>
      </div>
      <div :class="{ oor: fieldFlagged('inductance_q', cal, reference) }">
        <dt>Inductance Q</dt>
        <dd class="reported">{{ fmt(cal.inductance_q, 8) }} H</dd>
      </div>
      <div :class="{ oor: fieldFlagged('electrical_offset', cal, reference) }">
        <dt>Electrical offset</dt>
        <dd class="reported">{{ fmt(cal.electrical_offset, 6) }} rad</dd>
      </div>
      <div>
        <dt>Current-loop bandwidth</dt>
        <dd class="reported">
          {{ cfg ? `${cfg.bandwidth_active_hz} Hz active (${cfg.bandwidth_requested_hz} Hz requested)` : '—' }}
        </dd>
      </div>
      <div>
        <dt>Control rate / carrier</dt>
        <dd class="reported">
          {{ cfg ? `${cfg.control_rate_hz} Hz / ${cfg.carrier_hz} Hz` : '—' }}
        </dd>
      </div>
      <div>
        <dt>Duty</dt>
        <dd class="derived">{{ dutyPercent === null ? '—' : `${dutyPercent.toFixed(1)} %` }}</dd>
      </div>
      <div>
        <dt>Freshness</dt>
        <dd class="derived">{{ age(freshnessAgeMs) }}</dd>
      </div>
    </dl>
  </article>
</template>

<style scoped>
.card {
  background: #12181f;
  border: 1px solid #1d242c;
  border-radius: 8px;
  padding: 16px;
}
header {
  display: flex;
  justify-content: space-between;
  gap: 8px;
}
h2 {
  margin: 0;
  font-size: 14px;
}
.hint {
  font-size: 12px;
  color: #8b959f;
}
.warn {
  color: #e3b341;
}
dl {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px 16px;
  margin: 0;
}
dt {
  font-size: 11px;
  color: #6b7681;
}
dd {
  margin: 0;
  font-size: 13px;
}
.reported {
  color: #e6edf3;
}
.derived {
  font-style: italic;
  color: #7dd3c7;
}
.oor dd {
  outline: 1px solid #e3b341;
  padding: 0 4px;
}
.flag {
  font-size: 12px;
}
</style>
