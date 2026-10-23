#!/usr/bin/env python3
"""
The kernel for miatoll (SM7125): prebuilt/mach.development.bcm2837 with the EL1 virtual timer's
interrupt moved from INTID 27 to INTID 19.

osfmk/arm64/gic_v3_irq.c has GIC_INTID_VTIMER 27, where ARM's recommendation and SM7325 (redwood) put
it. SM7125, like the SDM845 and SC7180 it comes from, has the virtual timer on PPI 3 = INTID 19 (its
device tree's "arm,armv8-timer" node: <1 1 ..> <1 2 ..> <1 3 ..> <1 0 ..>). With 27 the kernel enables
an interrupt that never comes, and hangs at its first wait for the time (after "skip attaching fsw to
lo0 using legacy TX model"). The source change -- the INTID from the platform, not a constant -- needs a
kernel build; this is that change made to the prebuilt kernel, five instructions of gic_v3_irq.c:

  gic_v3_irq_cpu_init   mask (1 << 27) | SGI 0, 1     movk w8, #0x800, lsl #16  ->  movk w8, #0x8, lsl #16
                        mask (1 << 27), one CPU       mov  w8, #0x8000000       ->  mov  w8, #0x80000
                        GICR_IPRIORITYR(27)           strb w10, [x9, #0x39b]    ->  strb w10, [x9, #0x393]
                        the kprintf's number          mov  w9, #0x1b            ->  mov  w9, #0x13
  gic_irq_handler       intid == GIC_INTID_VTIMER     cmp  w20, #0x1b           ->  cmp  w20, #0x13

Every original instruction is checked before it is replaced: another kernel than the prebuilt one this
was made for is refused, not patched.

usage: tools/patch-kernel-miatoll.py [-i KERNEL] [-o out/mach.miatoll]
"""
import argparse, struct, sys

PATCHES = [  # virtual address, original, replacement (little-endian instruction words)
    (0xfffffff00747493c, '0800a172', '0801a072'),
    (0xfffffff007474944, '0800a152', '0801a052'),
    (0xfffffff007474968, '2a6d0e39', '2a4d0e39'),
    (0xfffffff0074749d4, '69038052', '69028052'),
    (0xfffffff007474b04, '9f6e0071', '9f4e0071'),
]

ap = argparse.ArgumentParser()
ap.add_argument('-i', '--input', default='prebuilt/mach.development.bcm2837')
ap.add_argument('-o', '--output', default='out/mach.miatoll')
args = ap.parse_args()

d = bytearray(open(args.input, 'rb').read())
if struct.unpack_from('<I', d, 0)[0] != 0xFEEDFACF:
    sys.exit(f'{args.input}: not a 64-bit Mach-O')
segments, off = [], 32
for _ in range(struct.unpack_from('<I', d, 16)[0]):
    cmd, size = struct.unpack_from('<II', d, off)
    if cmd == 0x19:                                     # LC_SEGMENT_64
        vmaddr, _, fileoff, filesize = struct.unpack_from('<QQQQ', d, off + 24)
        segments.append((vmaddr, fileoff, filesize))
    off += size


def file_offset(va):
    for vmaddr, fileoff, filesize in segments:
        if vmaddr <= va < vmaddr + filesize:
            return fileoff + va - vmaddr
    sys.exit(f'{args.input}: {va:#x} is in no segment')


for va, old, new in PATCHES:
    fo = file_offset(va)
    if d[fo:fo + 4] != bytes.fromhex(old):
        sys.exit(f'{args.input}: at {va:#x} {d[fo:fo + 4].hex()}, not {old}: not the kernel this patch is for')
for va, old, new in PATCHES:
    fo = file_offset(va)
    d[fo:fo + 4] = bytes.fromhex(new)
open(args.output, 'wb').write(d)
print(f'{args.output}: the virtual timer on INTID 19 ({len(PATCHES)} instructions)')
