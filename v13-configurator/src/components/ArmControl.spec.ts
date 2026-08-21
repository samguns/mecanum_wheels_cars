import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { describe, expect, it } from 'vitest'

import ArmControl from '@/components/ArmControl.vue'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'

function mountControl() {
  setActivePinia(createPinia())
  return mount(ArmControl)
}

describe('ArmControl', () => {
  it('acts on the selected motor and stays disabled until connected', async () => {
    const wrapper = mountControl()
    const session = useSessionStore()
    const device = useDeviceStore()
    expect(wrapper.find('button.arm').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).toMatch(/Arm/)
    expect(wrapper.text()).toMatch(/Disarm/)

    session.connection = 'ready'
    session.identity = {
      firmware_level: '002',
      protocol_version: 1,
      can_id: 0x202,
      motor_count: 2,
      config_version: 2,
      uptime_ms: 1000,
    }
    session.selectedMotor = 2
    await wrapper.vm.$nextTick()
    expect(wrapper.text()).toContain('Rear Left')
    expect(wrapper.find('button.arm').attributes('disabled')).toBeUndefined()

    device.mirror.motors = [
      {
        motor: 2,
        armed: true,
        mode: 'velocity',
        position_mrad: 0,
        velocity: 0,
        current_q: 0,
        timed_out: false,
        limit_causes: { current: false, output_voltage: null, bus_voltage: false },
        limit_count: 0,
        pair_fault: null,
      },
    ]
    await wrapper.vm.$nextTick()
    expect(wrapper.find('button.arm').attributes('disabled')).toBeDefined()
    expect(wrapper.find('button.disarm').attributes('disabled')).toBeUndefined()
  })
})
