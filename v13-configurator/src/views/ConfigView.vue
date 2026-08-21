<script setup lang="ts">
import { computed, ref } from 'vue'

import CalibrationPending from '@/components/CalibrationPending.vue'
import CalibrationProgress from '@/components/CalibrationProgress.vue'
import CalibrationStatus from '@/components/CalibrationStatus.vue'
import ConnectionPanel from '@/components/ConnectionPanel.vue'
import MotorParametersCard from '@/components/MotorParametersCard.vue'
import RefusalNotice from '@/components/RefusalNotice.vue'
import SettingsForm from '@/components/SettingsForm.vue'
import { wheelLabel } from '@/lib/wheel'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'

const device = useDeviceStore()
const session = useSessionStore()

const wheelsClear = ref(false)
const vehicleSecured = ref(false)
const acknowledged = ref(false)
const starting = ref(false)

const selectedWheel = computed(() =>
  wheelLabel(session.identity?.can_id ?? 0, session.selectedMotor),
)

const selectedCal = computed(() =>
  device.mirror.cal.find((c) => c.motor === session.selectedMotor),
)

const unmet = computed(() => {
  const reasons: string[] = []
  if (!session.canAct) reasons.push('no live identified session')
  if (!wheelsClear.value) reasons.push('wheels are not confirmed clear')
  if (!vehicleSecured.value) reasons.push('vehicle is not confirmed secured')
  if (!acknowledged.value) reasons.push('safety precondition is not acknowledged')
  if (device.mirror.motors.find((m) => m.motor !== session.selectedMotor)?.armed) {
    reasons.push('the unselected motor is armed')
  }
  return reasons
})

function calFor(motor: number) {
  return device.mirror.cal.find((c) => c.motor === motor) ?? null
}

async function startCalibration() {
  starting.value = true
  try {
    await session.confirmIntent(
      `${selectedWheel.value} wheels clear, vehicle secured, power cut within reach`,
    )
    await session.calibrateStart()
  } finally {
    starting.value = false
  }
}
</script>

<template>
  <div>
    <ConnectionPanel />

    <section v-if="session.refuseReason && session.connection === 'disconnected'" class="banner">
      <h2>Not a trusted device</h2>
      <p>{{ session.refuseReason }}</p>
      <p>No controller values are shown as live. Reconnect after flashing a supported firmware.</p>
    </section>

    <template v-else-if="session.canAct || session.connection === 'lost'">
      <div class="toolbar">
        <p class="reported">
          Last refresh:
          <span class="derived">
            {{
              device.lastRefreshedAt
                ? new Date(device.lastRefreshedAt).toLocaleTimeString()
                : 'never'
            }}
          </span>
          <span v-if="device.stale" class="warn"> · stale</span>
          <span v-if="device.writeUnknown" class="warn">
            · stored state unknown — re-read required
          </span>
        </p>
        <button type="button" :disabled="!session.canAct" @click="session.readAll()">
          Re-read
        </button>
      </div>

      <CalibrationStatus
        :motors="device.mirror.cal"
        :can-id="session.identity?.can_id ?? device.mirror.cfg?.can_id ?? 0"
      />

      <div class="cards">
        <template v-for="motor in [1, 2]" :key="motor">
          <MotorParametersCard
            v-if="calFor(motor)"
            :cal="calFor(motor)!"
            :cfg="device.mirror.cfg"
            :wheel-label="wheelLabel(session.identity?.can_id ?? 0, motor)"
            :reference="motor === 2 ? calFor(1) : null"
            :reference-label="motor === 2 ? wheelLabel(session.identity?.can_id ?? 0, 1) : ''"
            :freshness-age-ms="device.freshnessAgeMs"
            :duty-percent="device.derived.dutyPercent"
          />
        </template>
      </div>

      <section class="panel">
        <h2>Guided calibration — {{ selectedWheel }}</h2>
        <p class="warn">
          This energises {{ selectedWheel }} only. The other motor of the pair must stay
          de-energised.
        </p>
        <p v-if="!selectedCal?.valid" class="warn">
          {{ selectedWheel }} reports an incomplete or invalid stored calibration.
        </p>
        <label><input v-model="wheelsClear" type="checkbox" /> Wheels are clear of the ground</label>
        <label><input v-model="vehicleSecured" type="checkbox" /> Vehicle is secured</label>
        <label>
          <input v-model="acknowledged" type="checkbox" />
          I acknowledge the safety precondition for {{ selectedWheel }}
        </label>
        <ul v-if="unmet.length" class="pre">
          <li v-for="reason in unmet" :key="reason">Unmet before attempt: {{ reason }}</li>
        </ul>
        <RefusalNotice
          v-if="session.refuseReason && session.connection !== 'disconnected'"
          :reason="session.refuseReason"
          :motor="session.selectedMotor"
        />
        <button
          type="button"
          :disabled="unmet.length > 0 || starting || !session.canAct"
          @click="startCalibration"
        >
          Confirm and start calibration
        </button>
      </section>

      <CalibrationProgress
        v-if="device.progress || device.calPhase === 'failed'"
        :motor="device.progress?.motor ?? session.selectedMotor"
        :stage="device.progress?.stage ?? '—'"
        :percent="device.progress?.percent ?? 0"
        :energised="device.progress?.energised ?? false"
        :phase="device.calPhase"
        :reason="device.fault?.reason ?? null"
      />

      <CalibrationPending v-if="device.pending" :pending="device.pending" />
      <div v-if="device.pending" class="row">
        <button type="button" @click="session.calibrateAccept()">Accept and store</button>
        <button type="button" @click="session.calibrateReject()">Reject</button>
      </div>
      <p v-if="device.calPhase === 'resolved'" class="ok">
        Accept acknowledged. Values below are from a verification re-read.
      </p>

      <SettingsForm />
      <p class="legend">
        <span class="reported">Reported</span> values come from the controller.
        <span class="derived">Derived</span> marks wheel label, duty, freshness age, and the
        out-of-range flag. Unwritten operator edits (none on this view) would use a third style.
      </p>
    </template>
  </div>
</template>

<style scoped>
.banner {
  background: #2a1d12;
  border: 1px solid #e3b341;
  border-radius: 8px;
  padding: 16px;
}
.toolbar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
}
.panel {
  background: #12181f;
  border: 1px solid #1d242c;
  border-radius: 8px;
  padding: 16px;
  margin: 16px 0;
}
button {
  background: #16323a;
  color: #4fd6c1;
  border: 0;
  border-radius: 6px;
  padding: 6px 12px;
  cursor: pointer;
}
button:disabled {
  opacity: 0.45;
  cursor: default;
}
.cards {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}
.legend,
.reported {
  font-size: 12px;
  color: #8b959f;
}
.derived {
  font-style: italic;
  color: #7dd3c7;
}
.warn {
  color: #e3b341;
}
.ok {
  color: #4fd6c1;
}
.pre {
  color: #e3b341;
  font-size: 13px;
}
.row {
  display: flex;
  gap: 8px;
}
label {
  display: block;
  font-size: 13px;
  margin: 6px 0;
}
</style>
