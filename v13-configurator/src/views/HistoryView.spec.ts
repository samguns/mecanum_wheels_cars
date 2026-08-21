import { mount, flushPromises } from '@vue/test-utils'
import { describe, expect, it, vi } from 'vitest'

vi.mock('@tauri-apps/api/core', () => ({
  invoke: vi.fn().mockResolvedValue([
    {
      timestamp: '1',
      operator: 'bench-s10',
      can_id: 0x202,
      motor_index: 1,
      wheel_label: 'Rear Right',
      kind: 'setting_written',
      before: { busmin_mv: 7000 },
      after: { busmin_mv: 8000 },
    },
  ]),
}))

import HistoryView from '@/views/HistoryView.vue'

describe('HistoryView', () => {
  it('shows operator, target, and before/after values', async () => {
    const wrapper = mount(HistoryView)
    await flushPromises()
    expect(wrapper.text()).toContain('bench-s10')
    expect(wrapper.text()).toContain('Rear Right')
    expect(wrapper.text()).toContain('before')
    expect(wrapper.text()).toContain('8000')
    expect(wrapper.text()).toContain('7000')
  })
})
