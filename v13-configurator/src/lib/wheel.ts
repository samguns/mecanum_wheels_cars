/** Motor 1 is the right wheel, motor 2 the left. Node 0x01 front, 0x02 rear. */
export function wheelLabel(canId: number, motor: number): string {
  const node = canId & 0xff
  const pair = node === 0x01 ? 'Front' : node === 0x02 ? 'Rear' : `Node 0x${node.toString(16)}`
  const side = motor === 1 ? 'Right' : motor === 2 ? 'Left' : `M${motor}`
  return `${pair} ${side}`
}
