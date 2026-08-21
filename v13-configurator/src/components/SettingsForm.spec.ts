import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { describe, expect, it } from 'vitest'

import SettingsForm from '@/components/SettingsForm.vue'
import { useDeviceStore } from '@/stores/device'

function mountForm() {
  setActivePinia(createPinia())
  return mount(SettingsForm)
}

describe('SettingsForm', () => {
  it('rejects reversed bus window and armed writes', async () => {
    const wrapper = mountForm()
    const device = useDeviceStore()
    device.mirror.motors = [
      {
        motor: 1,
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
    await wrapper.find('input[type="checkbox"]').setValue(true)
    const buttons = wrapper.findAll('button')
    await buttons[2].trigger('click')
    expect(wrapper.text()).toMatch(/disarm/i)

    device.mirror.motors[0].armed = false
    const inputs = wrapper.findAll('input[type="number"]')
    await inputs[1].setValue(20000)
    await inputs[2].setValue(8000)
    expect(wrapper.text()).toMatch(/min must be less than max/)
  })

  it('states bandwidth bounds', async () => {
    const wrapper = mountForm()
    const bw = wrapper.findAll('input[type="number"]')[0]
    await bw.setValue(50)
    expect(wrapper.text()).toMatch(/100-10000/)
    await bw.setValue(10001)
    expect(wrapper.text()).toMatch(/100-10000/)
    await bw.setValue(100)
    expect(wrapper.text()).not.toMatch(/bandwidth must be 100-10000/)
  })

  it('states CAN id and bus-window board bounds', async () => {
    const wrapper = mountForm()
    await wrapper.find('input:not([type])').setValue('0')
    expect(wrapper.text()).toMatch(/0x001-0x7FF/)
    await wrapper.find('input:not([type])').setValue('202')
    const inputs = wrapper.findAll('input[type="number"]')
    await inputs[1].setValue(6000)
    await inputs[2].setValue(24000)
    expect(wrapper.text()).toMatch(/7000-24000/)
  })

  it('shows a before and after comparison', () => {
    const wrapper = mountForm()
    expect(wrapper.text()).toMatch(/Before:/)
    expect(wrapper.text()).toMatch(/After \(if confirmed\)/)
  })
})
