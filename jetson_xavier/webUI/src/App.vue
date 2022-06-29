<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { io, type Socket } from 'socket.io-client'

type JoystickState = {
  x: number
  y: number
}

type CanMessage = {
  id: string
  dlc: number
  data: number[]
  timestamp: number
  extended: boolean
}

type BusVoltageMessage = {
  canId: '0x201' | '0x202'
  busVoltage: number
  sourceStatusId: string
  timestamp: number
}

const leftJoystick = ref<JoystickState>({ x: 0, y: 0 })
const rightJoystick = ref<JoystickState>({ x: 0, y: 0 })

const SOCKET_SERVER_URL = `http://${window.location.hostname}:8080`
const JOYSTICK_COMMAND_EVENT = 'joystick_command'
const CAN_RX_EVENT = 'can_message'
const BUS_VOLTAGE_EVENT = 'bus_voltage'
const E_STOP_EVENT = 'estop'
const E_STOP_STATE_EVENT = 'estop_state'
const MAX_CAN_MESSAGES = 15
const canMessages = ref<CanMessage[]>([])
const busVoltageByCanId = ref<Record<'0x201' | '0x202', number | null>>({
  '0x201': null,
  '0x202': null,
})
const estopped = ref(false)
let socket: Socket | null = null

const DEADZONE = 0.05

const normalizeAxis = (value: number): number => {
  if (Math.abs(value) < DEADZONE) {
    return 0
  }
  return Math.max(-1, Math.min(1, value))
}

const command = computed(() => ({
  x: normalizeAxis(leftJoystick.value.x),
  y: normalizeAxis(leftJoystick.value.y),
  rotation: normalizeAxis(rightJoystick.value.x),
}))

onMounted(() => {
  socket = io(SOCKET_SERVER_URL, {
    transports: ['websocket'],
  })

  socket.on('connect', () => {
    socket?.emit(JOYSTICK_COMMAND_EVENT, command.value)
  })

  socket.on(CAN_RX_EVENT, (message: CanMessage) => {
    canMessages.value = [message, ...canMessages.value].slice(0, MAX_CAN_MESSAGES)
  })

  socket.on(BUS_VOLTAGE_EVENT, (message: BusVoltageMessage) => {
    busVoltageByCanId.value[message.canId] = message.busVoltage
  })

  socket.on(E_STOP_STATE_EVENT, (payload: { engaged: boolean }) => {
    estopped.value = Boolean(payload?.engaged)
  })
})

onBeforeUnmount(() => {
  socket?.off(CAN_RX_EVENT)
  socket?.off(BUS_VOLTAGE_EVENT)
  socket?.off(E_STOP_STATE_EVENT)
  socket?.disconnect()
  socket = null
})

watch(command, (value) => {
  if (!socket?.connected) {
    return
  }

  socket.emit(JOYSTICK_COMMAND_EVENT, value)
}, { deep: true })

const toggleEstop = () => {
  const newState = !estopped.value
  estopped.value = newState
  if (socket?.connected) {
    socket.emit(E_STOP_EVENT, newState)
  }
}

const toState = (event: Event): JoystickState => {
  const target = event.target as HTMLElement
  const force = Math.min(parseFloat(target.dataset.force || '0'), 1)
  const radian = parseFloat(target.dataset.radian || '0')
  return {
    x: Math.cos(radian) * force,
    y: Math.sin(radian) * force,
  }
}

const handleLeftMove = (event: Event) => {
  leftJoystick.value = toState(event)
}

const handleRightMove = (event: Event) => {
  rightJoystick.value = toState(event)
}

const handleLeftUp = () => {
  leftJoystick.value = { x: 0, y: 0 }
}

const handleRightUp = () => {
  rightJoystick.value = { x: 0, y: 0 }
}
</script>

<template>
  <div class="app-grid">
    <div class="top-row">
      <div class="can-container">
        <h3>Bus Voltage</h3>
        <table class="voltage-table">
          <thead>
            <tr>
              <th>CAN ID</th>
              <th>Bus Voltage (V)</th>
            </tr>
          </thead>
          <tbody>
            <tr>
              <td>0x201</td>
              <td>{{ busVoltageByCanId['0x201'] === null ? '-' : busVoltageByCanId['0x201']!.toFixed(2) }}</td>
            </tr>
            <tr>
              <td>0x202</td>
              <td>{{ busVoltageByCanId['0x202'] === null ? '-' : busVoltageByCanId['0x202']!.toFixed(2) }}</td>
            </tr>
          </tbody>
        </table>

        <h3>CAN RX</h3>
        <div class="can-log">
          <div v-if="canMessages.length === 0" class="can-empty">No CAN messages yet</div>
          <div v-for="(msg, index) in canMessages" :key="`${msg.timestamp}-${index}`" class="can-line">
            {{ msg.id }}
            | DLC {{ msg.dlc }}
            | {{ msg.data.map((byte) => byte.toString(16).toUpperCase().padStart(2, '0')).join(' ') }}
          </div>
        </div>
      </div>
    </div>

    <div class="middle-row">
      <div class="joystick-container">
        <h3>Left Stick</h3>
        <virtual-joystick
          data-mode="fixed"
          data-debug="true"
          @joystickmove="handleLeftMove"
          @joystickup="handleLeftUp"
          @joystickdown="handleLeftMove"
          style="width: 150px; height: 150px; position: relative;"
        ></virtual-joystick>
        <div class="debug">
          Translate X: {{ command.x.toFixed(2) }}<br>
          Translate Y: {{ command.y.toFixed(2) }}
        </div>
      </div>

      <div class="joystick-container">
        <h3>Right Stick</h3>
        <virtual-joystick
          data-mode="fixed"
          data-lock="y"
          @joystickmove="handleRightMove"
          @joystickup="handleRightUp"
          @joystickdown="handleRightMove"
          style="width: 150px; height: 150px; position: relative;"
        ></virtual-joystick>
        <div class="debug">
          Rotate (Z): {{ command.rotation.toFixed(2) }}
        </div>
      </div>
    </div>

    <button
      class="estop-button fixed"
      :class="{ engaged: estopped }"
      @click="toggleEstop"
      aria-pressed="false"
    >
      {{ estopped ? 'E-STOP: ENGAGED' : 'E-STOP: RELEASED' }}
    </button>
  </div>
</template>

<style scoped>
.app-grid {
  display: flex;
  flex-direction: column;
  height: 100vh;
  width: 100%;
  padding: 16px;
  box-sizing: border-box;
  background-color: #f0f0f0;
}

.top-row {
  display: flex;
  justify-content: center;
  margin-bottom: 18px;
}

.middle-row {
  display: flex;
  justify-content: space-evenly;
  align-items: center;
  flex: 1 1 auto;
}

.joystick-container {
  display: flex;
  flex-direction: column;
  align-items: center;
}

.debug {
  margin-top: 10px;
  font-family: monospace;
}

.can-container {
  width: 380px;
  max-height: 300px;
  display: flex;
  flex-direction: column;
}

.estop-container {
  display: flex;
  align-items: center;
  justify-content: center;
}

.estop-button {
  padding: 12px 18px;
  border-radius: 8px;
  border: 2px solid #800000;
  background: #ffdddd;
  color: #660000;
  font-weight: 700;
  cursor: pointer;
}

.estop-button.engaged {
  background: #660000;
  color: #ffffff;
  box-shadow: 0 0 8px rgba(255, 0, 0, 0.6);
}

.estop-button.fixed {
  position: fixed;
  right: 20px;
  bottom: 20px;
  z-index: 1000;
}

.can-log {
  margin-top: 10px;
  padding: 8px;
  background: rgba(0, 0, 0, 0.05);
  border-radius: 8px;
  font-family: monospace;
  font-size: 12px;
  overflow: auto;
}

.voltage-table {
  width: 100%;
  border-collapse: collapse;
  font-family: monospace;
  font-size: 13px;
  background: rgba(0, 0, 0, 0.05);
  border-radius: 8px;
  overflow: hidden;
}

.voltage-table th,
.voltage-table td {
  padding: 8px;
  border-bottom: 1px solid rgba(0, 0, 0, 0.1);
  text-align: left;
}

.can-line {
  margin-bottom: 4px;
  white-space: nowrap;
}

.can-empty {
  opacity: 0.7;
}

/* Ensure joysticks are visible and removes the default width and height */
virtual-joystick {
  touch-action: none;
  /* background: rgba(0, 0, 0, 0.1); */
  border-radius: 50%;
  /* remove default width and height to allow inline styles to take effect */
  width: auto !important;
  height: auto !important;
}
</style>
