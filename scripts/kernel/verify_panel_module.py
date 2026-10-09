#!/usr/bin/env python3
"""Verify the R1 panel's binary data contract with the retained framebuffer driver."""
import argparse
import hashlib
import json
import pathlib
import struct


def panel_data(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 8:
        raise ValueError('expected a little-endian ELF32 MIPS module')
    shoff = struct.unpack_from('<I', data, 32)[0]
    entsize, count = struct.unpack_from('<HH', data, 46)
    if entsize != 40:
        raise ValueError('unexpected section-header size')
    sections = [struct.unpack_from('<10I', data, shoff + i * entsize) for i in range(count)]
    for section in sections:
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        if section[9] != 16:
            raise ValueError('unexpected symbol-table entry size')
        for offset in range(section[4], section[4] + section[5], 16):
            name, value, size, info, other, index = struct.unpack_from('<IIIBBH', data, offset)
            if names[name:].split(b'\0', 1)[0] != b'lcdc_data':
                continue
            if size != 168:
                raise ValueError(f'lcdc_data must be 168 bytes, got {size}')
            target = sections[index]
            if target[1] != 1 or value + size > target[5]:
                raise ValueError('panel symbol is not contained in initialized data')
            result = data[target[4] + value:target[4] + value + size]
            if result[160:168] != b'\0' * 8:
                raise ValueError('optional callback slots at 160/164 must both be NULL')
            return result
    raise ValueError('missing lcdc_data symbol')


def verify(vendor, candidate):
    baseline, rebuilt = panel_data(vendor), panel_data(candidate)
    if baseline[:152] != rebuilt[:152]:
        raise ValueError('panel timing/configuration fields differ from the vendor contract')
    return {'valid': True, 'size': 168, 'power_callback_offsets': [152, 156],
            'null_optional_callback_offsets': [160, 164], 'timings_match_vendor': True,
            'candidate_sha256': hashlib.sha256(candidate.read_bytes()).hexdigest(),
            'vendor_sha256': hashlib.sha256(vendor.read_bytes()).hexdigest()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vendor-module', required=True, type=pathlib.Path)
    parser.add_argument('--candidate-module', required=True, type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    try:
        result = verify(args.vendor_module, args.candidate_module)
    except (OSError, ValueError, struct.error, IndexError) as error:
        parser.exit(1, f'panel ABI verification failed: {error}\n')
    rendered = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end='')
