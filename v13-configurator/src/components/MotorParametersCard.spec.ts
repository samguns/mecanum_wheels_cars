import { mount } from '@vue/test-utils'
import { describe, expect, it } from 'vitest'

import MotorParametersCard from '@/components/MotorParametersCard.vue'
import type { CalRecord } from '@/types/protocol'

const baseCal: CalRecord = {
  motor: 1,
  aligned: true,
  characterised: true,
  pole_pairs: 7,
  direction: 1,
  electrical_offset: 2.094,
  phase_resistance: 0.084,
  inductance_d: 0.000125,
  inductance_q: 0.000131,
  valid: true,
}

function mountCard(cal: CalRecord = baseCal) {
  return mount(MotorParametersCard, {
    props: {
      cal,
      cfg: {
        can_id: 0x202,
        bandwidth_requested_hz: 1000,
        bandwidth_active_hz: 1000,
        bandwidth_clamped: false,
        control_rate_hz: 10000,
        carrier_hz: 20000,
        decimation: 2,
        mode: ['velocity', 'velocity'],
        bus_min_mv: 7000,
        bus_max_mv: 24000,
        calibrated: true,
      },
      wheelLabel: 'Rear Right',
      freshnessAgeMs: 1500,
      dutyPercent: 12.5,
    },
  })
}

describe('MotorParametersCard', () => {
  it('renders units and no editable controls', () => {
    const wrapper = mountCard()
    expect(wrapper.text()).toContain('Ω')
    expect(wrapper.text()).toContain('H')
    expect(wrapper.text()).toContain('rad')
    expect(wrapper.text()).toContain('requires running calibration')
    expect(wrapper.find('input').exists()).toBe(false)
    expect(wrapper.find('textarea').exists()).toBe(false)
    expect(wrapper.find('select').exists()).toBe(false)
  })

  it('flags an out-of-range reported value without clamping it', () => {
    const wrapper = mountCard({ ...baseCal, pole_pairs: 99 })
    expect(wrapper.text()).toContain('99')
    expect(wrapper.text()).toContain('out of range: yes')
    expect(wrapper.find('.oor').exists()).toBe(true)
  })

  it('flags a left-wheel pole-pair that does not match the right-wheel reference', () => {
    const wrapper = mount(MotorParametersCard, {
      props: {
        cal: { ...baseCal, motor: 2, pole_pairs: 6, direction: -1 },
        cfg: null,
        wheelLabel: 'Rear Left',
        reference: { ...baseCal, motor: 1, pole_pairs: 7 },
        referenceLabel: 'Rear Right',
        freshnessAgeMs: 100,
        dutyPercent: null,
      },
    })
    expect(wrapper.text()).toContain('out of range: yes')
    expect(wrapper.text()).toMatch(/Differs from Rear Right/)
    expect(wrapper.find('.oor').exists()).toBe(true)
  })

  it('marks derived quantities separately from reported ones', () => {
    const wrapper = mountCard()
    const derived = wrapper.findAll('.derived').map((n) => n.text()).join(' ')
    expect(derived).toContain('Rear Right')
    expect(derived).toContain('12.5 %')
    expect(derived).toMatch(/1\.5 s|1500 ms/)
    expect(wrapper.find('.reported').text()).toContain('requires running calibration')
  })
})
