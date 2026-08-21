import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { describe, expect, it } from 'vitest'

import ImpedancePanel from '@/components/ImpedancePanel.vue'
import { useDeviceStore } from '@/stores/device'
import { useSessionStore } from '@/stores/session'

function mountPanel() {
  setActivePinia(createPinia())
  const wrapper = mount(ImpedancePanel)
  const session = useSessionStore()
  const device = useDeviceStore()
  session.connection = 'ready'
  session.selectedMotor = 1
  device.mirror.motors = [
    {
      motor: 1,
      armed: false,
      mode: 'impedance',
      position_mrad: 0,
      velocity: 0,
      current_q: 0,
      timed_out: false,
      limit_causes: { current: false, output_voltage: false, bus_voltage: false },
      limit_count: 0,
      pair_fault: false,
    },
  ]
  return wrapper
}

describe('ImpedancePanel', () => {
  it('states the five-term ranges before submit', async () => {
    const wrapper = mountPanel()
    const inputs = wrapper.findAll('input[type="number"]')
    await inputs[0].setValue(700000)
    await wrapper.find('button').trigger('click')
    expect(wrapper.text()).toMatch(/-628319 to 628319/)

    await inputs[0].setValue(0)
    await inputs[2].setValue(80)
    await wrapper.find('button').trigger('click')
    expect(wrapper.text()).toMatch(/kp must be 0 to 50/)
  })

  it('refuses apply when the selected motor is in velocity mode', async () => {
    const wrapper = mountPanel()
    const device = useDeviceStore()
    device.mirror.motors[0].mode = 'velocity'
    await wrapper.vm.$nextTick()
    expect(wrapper.find('button').attributes('disabled')).toBeDefined()
    expect(wrapper.text()).toMatch(/not in impedance mode/)
  })
})
