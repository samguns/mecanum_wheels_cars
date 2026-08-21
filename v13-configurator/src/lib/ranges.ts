import type { CalRecord } from '@/types/protocol'

/** Motor 1 (right wheel) is the spec reference for the other motor on the same controller. */
export const PAIR_REFERENCE_MOTOR = 1

/** R / Ld / Lq may differ by this fraction of the reference and still count as the same spec. */
export const PAIR_CHARACTERISTIC_TOLERANCE = 0.25

export const RANGES = {
  pole_pairs: { min: 1, max: 64, unit: '' },
  direction: { min: -1, max: 1, unit: '' },
  electrical_offset: { min: 0, max: Math.PI * 2, unit: 'rad' },
  phase_resistance: { min: 0.01, max: 100, unit: 'Ω' },
  inductance: { min: 1e-6, max: 0.1, unit: 'H' },
  p_des_mrad: { min: -628319, max: 628319, unit: 'mrad' },
  v_des: { min: -45, max: 45, unit: 'rad/s' },
  kp: { min: 0, max: 50, unit: '' },
  kd: { min: 0, max: 1, unit: '' },
  t_ff: { min: -0.5, max: 0.5, unit: 'Nm' },
} as const

export function inRange(value: number, min: number, max: number): boolean {
  return Number.isFinite(value) && value >= min && value <= max
}

export function calibrationOutOfRange(cal: CalRecord): boolean {
  if (!inRange(cal.pole_pairs, RANGES.pole_pairs.min, RANGES.pole_pairs.max)) return true
  if (cal.direction !== 1 && cal.direction !== -1) return true
  if (!inRange(cal.electrical_offset, RANGES.electrical_offset.min, RANGES.electrical_offset.max)) {
    return true
  }
  if (!inRange(cal.phase_resistance, RANGES.phase_resistance.min, RANGES.phase_resistance.max)) {
    return true
  }
  if (!inRange(cal.inductance_d, RANGES.inductance.min, RANGES.inductance.max)) return true
  if (!inRange(cal.inductance_q, RANGES.inductance.min, RANGES.inductance.max)) return true
  return false
}

export function fieldOutOfRange(key: keyof typeof RANGES | 'inductance_d' | 'inductance_q', value: number): boolean {
  if (key === 'inductance_d' || key === 'inductance_q') {
    return !inRange(value, RANGES.inductance.min, RANGES.inductance.max)
  }
  if (key === 'direction') return value !== 1 && value !== -1
  const range = RANGES[key]
  return !inRange(value, range.min, range.max)
}

function relativeMismatch(value: number, reference: number, tolerance = PAIR_CHARACTERISTIC_TOLERANCE): boolean {
  if (!Number.isFinite(value) || !Number.isFinite(reference) || reference === 0) return false
  return Math.abs(value - reference) / Math.abs(reference) > tolerance
}

export type PairCompareField = 'pole_pairs' | 'phase_resistance' | 'inductance_d' | 'inductance_q'

/** Direction and electrical offset are mount-specific. Pole pairs must match; R and L must be close. */
export function pairFieldMismatch(
  cal: CalRecord,
  reference: CalRecord | null | undefined,
  field: PairCompareField,
): boolean {
  if (!reference || cal.motor === reference.motor) return false
  if (field === 'pole_pairs') {
    if (!reference.aligned || !cal.aligned) return false
    return cal.pole_pairs !== reference.pole_pairs
  }
  if (!reference.characterised || !cal.characterised) return false
  if (field === 'phase_resistance') return relativeMismatch(cal.phase_resistance, reference.phase_resistance)
  if (field === 'inductance_d') return relativeMismatch(cal.inductance_d, reference.inductance_d)
  return relativeMismatch(cal.inductance_q, reference.inductance_q)
}

export function pairMismatched(cal: CalRecord, reference: CalRecord | null | undefined): boolean {
  return (
    pairFieldMismatch(cal, reference, 'pole_pairs') ||
    pairFieldMismatch(cal, reference, 'phase_resistance') ||
    pairFieldMismatch(cal, reference, 'inductance_d') ||
    pairFieldMismatch(cal, reference, 'inductance_q')
  )
}

export function fieldFlagged(
  key: PairCompareField | 'direction' | 'electrical_offset',
  cal: CalRecord,
  reference?: CalRecord | null,
): boolean {
  if (key === 'pole_pairs' || key === 'phase_resistance' || key === 'inductance_d' || key === 'inductance_q') {
    return fieldOutOfRange(key, cal[key]) || pairFieldMismatch(cal, reference, key)
  }
  if (key === 'direction') return fieldOutOfRange('direction', cal.direction)
  return fieldOutOfRange('electrical_offset', cal.electrical_offset)
}
