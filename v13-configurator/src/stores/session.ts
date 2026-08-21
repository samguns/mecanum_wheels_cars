import { invoke } from '@tauri-apps/api/core'
import { listen, type UnlistenFn } from '@tauri-apps/api/event'
import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

import { useDeviceStore } from '@/stores/device'
import type {
  BusRecord,
  CalPendRecord,
  CalProgRecord,
  CalRecord,
  CfgRecord,
  ConnectionState,
  ConnectResult,
  ControllerSnapshot,
  FaultRecord,
  IdRecord,
  ImpRecord,
  MotorRecord,
  PortDescriptor,
  SessionError,
  TelemetryResult,
  TimingRecord,
} from '@/types/protocol'

function asSessionError(err: unknown): SessionError {
  if (err && typeof err === 'object' && 'kind' in err && 'reason' in err) {
    return err as SessionError
  }
  return { kind: 'protocol', reason: String(err) }
}

export const useSessionStore = defineStore('session', () => {
  const ports = ref<PortDescriptor[]>([])
  const selectedPort = ref('')
  const connection = ref<ConnectionState>('disconnected')
  const identity = ref<IdRecord | null>(null)
  const resetForced = ref(false)
  const lastError = ref<SessionError | null>(null)
  const refuseReason = ref<string | null>(null)
  const selectedMotor = ref<1 | 2>(1)
  const appliedTelemetryMs = ref(0)
  const unlisteners: UnlistenFn[] = []

  const canAct = computed(
    () => connection.value === 'ready' || connection.value === 'busy',
  )

  async function ensureEvents() {
    if (unlisteners.length) return
    try {
      unlisteners.push(
        await listen('connection_changed', (event) => {
          const payload = event.payload as {
            state?: ConnectionState
            event?: string
            identity?: IdRecord | null
            reason?: string | null
            reset_forced?: boolean
          }
          const state = payload.state
          if (state) connection.value = state
          if (payload.identity) identity.value = payload.identity
          if (payload.reason) refuseReason.value = payload.reason
          if (payload.reset_forced) resetForced.value = true
          if (state === 'lost' || state === 'disconnected') {
            useDeviceStore().markStale()
          }
        }),
      )
      unlisteners.push(
        await listen('telemetry', (event) => {
          const payload = event.payload as { record?: { type?: string } & Record<string, unknown> }
          const record = payload.record
          if (!record?.type) return
          const device = useDeviceStore()
          device.noteLive()
          if (record.type === 'cal') device.applyCal(record as unknown as CalRecord)
          if (record.type === 'cfg') device.applyCfg(record as unknown as CfgRecord)
          if (record.type === 'motor') device.applyMotor(record as unknown as MotorRecord)
          if (record.type === 'imp') device.applyImp(record as unknown as ImpRecord)
          if (record.type === 'timing') device.applyTiming(record as unknown as TimingRecord)
          if (record.type === 'bus') device.applyBus(record as unknown as BusRecord)
        }),
      )
      unlisteners.push(
        await listen('calibration_progress', (event) => {
          const payload = event.payload as { record?: CalProgRecord }
          if (payload.record) useDeviceStore().applyProgress(payload.record)
        }),
      )
      unlisteners.push(
        await listen('calibration_pending', (event) => {
          const payload = event.payload as { record?: CalPendRecord }
          if (payload.record) useDeviceStore().applyPending(payload.record)
        }),
      )
      unlisteners.push(
        await listen('fault', (event) => {
          const payload = event.payload as { record?: FaultRecord }
          if (payload.record) useDeviceStore().applyFault(payload.record)
        }),
      )
      unlisteners.push(
        await listen('staleness', () => {
          useDeviceStore().markStale()
        }),
      )
      unlisteners.push(
        await listen('protocol_error', (event) => {
          const payload = event.payload as { detail?: string }
          if (payload.detail?.includes('timeout')) {
            useDeviceStore().markWriteUnknown()
          }
        }),
      )
    } catch {
      /* running outside Tauri */
    }
  }

  async function listPorts() {
    await ensureEvents()
    try {
      ports.value = await invoke<PortDescriptor[]>('list_ports')
    } catch {
      ports.value = []
    }
  }

  async function connect() {
    lastError.value = null
    refuseReason.value = null
    resetForced.value = false
    if (!selectedPort.value) {
      lastError.value = { kind: 'protocol', reason: 'select a port' }
      return
    }
    connection.value = 'identifying'
    try {
      const result = await invoke<ConnectResult>('connect', {
        args: { port: selectedPort.value },
      })
      identity.value = result.identity
      resetForced.value = result.reset_forced
      connection.value = 'ready'
      await readAll()
    } catch (err) {
      const error = asSessionError(err)
      lastError.value = error
      refuseReason.value = error.reason
      connection.value = 'disconnected'
      identity.value = null
      useDeviceStore().clear()
    }
  }

  async function disconnect() {
    await invoke('disconnect')
    connection.value = 'disconnected'
    identity.value = null
    useDeviceStore().clear()
  }

  async function readAll() {
    const snapshot = await invoke<ControllerSnapshot>('read_all')
    useDeviceStore().applySnapshot(snapshot)
    if (snapshot.identity) identity.value = snapshot.identity
  }

  async function abort() {
    await invoke('abort')
    useDeviceStore().clearPending()
    useDeviceStore().calPhase = 'unknown'
  }

  async function arm() {
    refuseReason.value = null
    lastError.value = null
    try {
      await invoke('arm', { args: { motor: selectedMotor.value } })
      await readAll()
    } catch (err) {
      const error = asSessionError(err)
      lastError.value = error
      refuseReason.value = error.reason
    }
  }

  async function disarm() {
    refuseReason.value = null
    lastError.value = null
    try {
      await invoke('disarm', { args: { motor: selectedMotor.value } })
      await readAll()
    } catch (err) {
      const error = asSessionError(err)
      lastError.value = error
      refuseReason.value = error.reason
    }
  }

  async function selectMotor(motor: 1 | 2) {
    if (useDeviceStore().pending && motor !== selectedMotor.value) {
      const discard = window.confirm(
        'A pending calibration result is not stored. Changing motor discards it. Continue?',
      )
      if (!discard) return
      useDeviceStore().clearPending()
    }
    selectedMotor.value = motor
    try {
      await invoke('select_motor', { args: { motor } })
    } catch {
      /* selection is local if Rust is unavailable */
    }
  }

  async function confirmIntent(precondition: string) {
    const canId = identity.value?.can_id ?? 0
    await invoke('confirm_intent', {
      args: {
        motor: selectedMotor.value,
        wheel_name: `${canId.toString(16)} M${selectedMotor.value}`,
        precondition,
      },
    })
  }

  async function calibrateStart() {
    refuseReason.value = null
    try {
      await invoke('calibrate_start')
      useDeviceStore().calPhase = 'running'
    } catch (err) {
      const error = asSessionError(err)
      lastError.value = error
      refuseReason.value = error.reason
    }
  }

  async function calibrateAccept() {
    refuseReason.value = null
    try {
      await invoke('calibrate_accept')
      useDeviceStore().clearPending()
      useDeviceStore().calPhase = 'resolved'
      await readAll()
    } catch (err) {
      const error = asSessionError(err)
      refuseReason.value = error.reason
      if (error.kind !== 'refused') useDeviceStore().calPhase = 'unknown'
    }
  }

  async function calibrateReject() {
    await invoke('calibrate_reject')
    useDeviceStore().clearPending()
    useDeviceStore().calPhase = 'idle'
  }

  async function applyImpedance(args: {
    motor: 1 | 2
    p_des_mrad: number
    v_des: number
    kp: number
    kd: number
    t_ff: number
  }) {
    refuseReason.value = null
    lastError.value = null
    try {
      await invoke('apply_impedance', { args })
      await readAll()
    } catch (err) {
      const error = asSessionError(err)
      lastError.value = error
      refuseReason.value = error.reason
      throw error
    }
  }

  async function setTelemetry(periodMs: number) {
    const result = await invoke<TelemetryResult>('set_telemetry', {
      args: { period_ms: periodMs },
    })
    appliedTelemetryMs.value = result.applied_period_ms
    useDeviceStore().appliedTelemetryMs = result.applied_period_ms
    return result.applied_period_ms
  }

  return {
    ports,
    selectedPort,
    connection,
    identity,
    resetForced,
    lastError,
    refuseReason,
    selectedMotor,
    appliedTelemetryMs,
    canAct,
    listPorts,
    connect,
    disconnect,
    readAll,
    abort,
    arm,
    disarm,
    applyImpedance,
    selectMotor,
    confirmIntent,
    calibrateStart,
    calibrateAccept,
    calibrateReject,
    setTelemetry,
  }
})
