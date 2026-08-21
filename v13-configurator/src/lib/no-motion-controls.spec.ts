import { readdirSync, readFileSync, statSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function walk(dir: string, acc: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name)
    if (statSync(path).isDirectory()) walk(path, acc)
    else if (/\.(vue|ts)$/.test(name) && !name.endsWith('.spec.ts')) acc.push(path)
  }
  return acc
}

describe('SC-010 no vehicle motion controls', () => {
  it('does not invoke arm or expose a motion setpoint', () => {
    const root = join(__dirname, '..')
    const files = walk(root)
    const banned = [
      /target_vel|velocity_setpoint|torque_setpoint|cmd_vx|joystick/i,
    ]
    for (const file of files) {
      const text = readFileSync(file, 'utf8')
      for (const pattern of banned) {
        expect(text, `${file} matches ${pattern}`).not.toMatch(pattern)
      }
    }
  })
})
