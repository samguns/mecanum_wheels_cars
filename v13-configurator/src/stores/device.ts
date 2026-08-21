import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

import { calibrationOutOfRange, PAIR_REFERENCE_MOTOR, pairMismatched } from '@/lib/ranges'
import { wheelLabel } from '@/lib/wheel'
import type {
  BusRecord,
  CalPendRecord,
  CalProgRecord,
  CalRecord,
  CfgRecord,
  ControllerSnapshot,
  DeviceMirror,
  FaultRecord,
  IdRecord,
  ImpRecord,
  MotorRecord,
  TimingRecord,
} from '@/types/protocol'

const STALE_MS = 2000

export const useDeviceStore = defineStore('device', () => {
  const identity = ref<IdRecord | null>(null)
  const mirror = ref<DeviceMirror>({
    cal: [],
    cfg: null,
    motors: [],
    impedance: [],
    timing: null,
    bus: null,
  })
  const lastRefreshedAt = ref<number | null>(null)
  const lastTelemetryAt = ref<number | null>(null)
  const stale = ref(true)
  const writeUnknown = ref(false)
  const progress = ref<CalProgRecord | null>(null)
  const pending = ref<CalPendRecord | null>(null)
  const fault = ref<FaultRecord | null>(null)
  const calPhase = ref('idle')
  const appliedTelemetryMs = ref(0)
  let staleTimer: ReturnType<typeof setInterval> | undefined

  function ensureStaleWatch() {
    if (staleTimer) return
    staleTimer = setInterval(() => {
      if (lastTelemetryAt.value !== null && Date.now() - lastTelemetryAt.value >= STALE_MS) {
        stale.value = true
      }
    }, 250)
  }

  function applySnapshot(snapshot: ControllerSnapshot) {
    identity.value = snapshot.identity
    mirror.value = snapshot.mirror
    lastRefreshedAt.value = Date.now()
    lastTelemetryAt.value = Date.now()
    stale.value = false
    writeUnknown.value = false
    ensureStaleWatch()
  }

  function applyCal(record: CalRecord) {
    mirror.value.cal = [...mirror.value.cal.filter((c) => c.motor !== record.motor), record].sort(
      (a, b) => a.motor - b.motor,
    )
  }

  function applyMotor(record: MotorRecord) {
    mirror.value.motors = [
      ...mirror.value.motors.filter((m) => m.motor !== record.motor),
      record,
    ].sort((a, b) => a.motor - b.motor)
  }

  function applyImp(record: ImpRecord) {
    mirror.value.impedance = [
      ...mirror.value.impedance.filter((m) => m.motor !== record.motor),
      record,
    ].sort((a, b) => a.motor - b.motor)
  }

  function applyCfg(record: CfgRecord) {
    mirror.value.cfg = record
  }

  function applyTiming(record: TimingRecord) {
    mirror.value.timing = record
  }

  function applyBus(record: BusRecord) {
    mirror.value.bus = record
  }

  function noteLive() {
    lastTelemetryAt.value = Date.now()
    stale.value = false
    ensureStaleWatch()
  }

  function markStale() {
    stale.value = true
  }

  function markWriteUnknown() {
    writeUnknown.value = true
    stale.value = true
  }

  function applyProgress(record: CalProgRecord) {
    progress.value = record
    calPhase.value = 'running'
    noteLive()
  }

  function applyPending(record: CalPendRecord) {
    pending.value = record
    calPhase.value = 'pending'
    noteLive()
  }

  function applyFault(record: FaultRecord) {
    fault.value = record
    if (record.kind === 'calibration') {
      calPhase.value = 'failed'
    }
  }

  function clearPending() {
    pending.value = null
  }

  function clear() {
    identity.value = null
    mirror.value = { cal: [], cfg: null, motors: [], impedance: [], timing: null, bus: null }
    lastRefreshedAt.value = null
    lastTelemetryAt.value = null
    stale.value = true
    writeUnknown.value = false
    progress.value = null
    pending.value = null
    fault.value = null
    calPhase.value = 'idle'
  }

  const freshnessAgeMs = computed(() =>
    lastRefreshedAt.value === null ? null : Date.now() - lastRefreshedAt.value,
  )

  const derived = computed(() => {
    const canId = identity.value?.can_id ?? mirror.value.cfg?.can_id ?? 0
    return {
      wheelLabels: [1, 2].map((m) => wheelLabel(canId, m)),
      dutyPercent: mirror.value.timing?.duty_percent ?? null,
      freshnessAgeMs: freshnessAgeMs.value,
      outOfRange: mirror.value.cal.map((c) => {
        const reference = mirror.value.cal.find((r) => r.motor === PAIR_REFERENCE_MOTOR) ?? null
        return {
          motor: c.motor,
          flag: calibrationOutOfRange(c) || pairMismatched(c, reference),
        }
      }),
    }
  })

  return {
    identity,
    mirror,
    lastRefreshedAt,
    lastTelemetryAt,
    stale,
    writeUnknown,
    progress,
    pending,
    fault,
    calPhase,
    appliedTelemetryMs,
    freshnessAgeMs,
    derived,
    applySnapshot,
    applyCal,
    applyMotor,
    applyImp,
    applyCfg,
    applyTiming,
    applyBus,
    noteLive,
    markStale,
    markWriteUnknown,
    applyProgress,
    applyPending,
    applyFault,
    clearPending,
    clear,
  }
})
